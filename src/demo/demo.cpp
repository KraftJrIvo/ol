#include "demo/demo.h"
#include "demo/menu.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace ol {

static void record_current_world_state(DemoApp* app);
static void save_profile(DemoApp* app);
static CameraView make_player_camera_view(DemoApp* app, Dimension* dim, const PlayerEntity* player);
static bool sync_session_from_network(DemoApp* app, Dimension* dim, PlayerEntity* player);
static bool update_network_state(DemoApp* app, Dimension* dim, PlayerEntity* player);

static void add_text_char(char* text, size_t cap, int c) {
    if (c < 32 || c > 126) return;
    const size_t len = std::strlen(text);
    if (len + 1 >= cap) return;
    text[len] = static_cast<char>(c);
    text[len + 1] = 0;
}

static void remove_text_char(char* text) {
    const size_t len = std::strlen(text);
    if (len > 0) text[len - 1] = 0;
}

static char* active_menu_text(DemoApp* app, size_t* out_cap) {
    if (app->active_menu_field == menu_input_player) {
        if (out_cap) *out_cap = sizeof(app->player_name);
        return app->player_name;
    }
    if (app->active_menu_field == menu_input_session) {
        if (out_cap) *out_cap = sizeof(app->session_name);
        return app->session_name;
    }
    if (out_cap) *out_cap = 0;
    return nullptr;
}

static void set_player_color_component(DemoApp* app, u32 control, int value) {
    value = static_cast<int>(clampf(static_cast<float>(value), 0.0f, 255.0f));
    if (control == menu_control_color_r) app->player_color.r = static_cast<unsigned char>(value);
    if (control == menu_control_color_g) app->player_color.g = static_cast<unsigned char>(value);
    if (control == menu_control_color_b) app->player_color.b = static_cast<unsigned char>(value);
}

static void copy_text(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    std::snprintf(dst, cap, "%s", src ? src : "");
}

static bool key_in_history_range(int key) {
    return key >= 0 && key < 512;
}

static bool frame_key_pressed(DemoApp* app, int key, const std::array<bool, 512>& queued_keys) {
    const bool down = IsKeyDown(key);
    const bool queued = key_in_history_range(key) ? queued_keys[static_cast<u32>(key)] : false;
    const bool previous = key_in_history_range(key) ? app->previous_key_down[static_cast<u32>(key)] : false;
    return queued || IsKeyPressed(key) || (down && !previous);
}

static void update_previous_key(DemoApp* app, int key) {
    if (key_in_history_range(key)) {
        app->previous_key_down[static_cast<u32>(key)] = IsKeyDown(key);
    }
}

static void capture_frame_input(DemoApp* app) {
    std::array<bool, 512> queued_keys{};
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
        if (key_in_history_range(key)) queued_keys[static_cast<u32>(key)] = true;
    }

    DemoFrameInput input{};
    const bool left_down = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
    const bool right_down = IsMouseButtonDown(MOUSE_RIGHT_BUTTON);
    input.mouse_left_down = left_down;
    input.mouse_left_pressed = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) || (left_down && !app->previous_mouse_left_down);
    input.mouse_left_released = IsMouseButtonReleased(MOUSE_LEFT_BUTTON) || (!left_down && app->previous_mouse_left_down);
    input.mouse_right_down = right_down;

    input.tab_pressed = frame_key_pressed(app, KEY_TAB, queued_keys);
    input.backspace_pressed = frame_key_pressed(app, KEY_BACKSPACE, queued_keys);
    input.escape_pressed = frame_key_pressed(app, KEY_ESCAPE, queued_keys);
    input.enter_pressed = frame_key_pressed(app, KEY_ENTER, queued_keys);
    input.plus_pressed = frame_key_pressed(app, KEY_EQUAL, queued_keys) || frame_key_pressed(app, KEY_KP_ADD, queued_keys);
    input.minus_pressed = frame_key_pressed(app, KEY_MINUS, queued_keys) || frame_key_pressed(app, KEY_KP_SUBTRACT, queued_keys);
    input.f3_pressed = frame_key_pressed(app, KEY_F3, queued_keys);
    input.p_pressed = frame_key_pressed(app, KEY_P, queued_keys);
    input.f11_pressed = frame_key_pressed(app, KEY_F11, queued_keys);
    input.r_pressed = frame_key_pressed(app, KEY_R, queued_keys);
    input.c_pressed = frame_key_pressed(app, KEY_C, queued_keys);
    input.o_pressed = frame_key_pressed(app, KEY_O, queued_keys);
    input.space_pressed = frame_key_pressed(app, KEY_SPACE, queued_keys);

    app->frame_input = input;
    app->previous_mouse_left_down = left_down;

    const int tracked_keys[] = {
        KEY_TAB, KEY_BACKSPACE, KEY_ESCAPE, KEY_ENTER, KEY_EQUAL, KEY_KP_ADD,
        KEY_MINUS, KEY_KP_SUBTRACT, KEY_F3, KEY_P, KEY_F11, KEY_R, KEY_C, KEY_O, KEY_SPACE
    };
    for (int key : tracked_keys) update_previous_key(app, key);
}

static constexpr const char* world_playground_name = "playground";
static constexpr const char* world_hills_name = "hills";
static constexpr const char* world_cave_name = "cave";
static constexpr const char* world_choices[] = {
    world_playground_name,
    world_hills_name,
    world_cave_name,
};
static constexpr u32 world_choice_count = static_cast<u32>(sizeof(world_choices) / sizeof(world_choices[0]));

static const char* canonical_world_name(const char* name) {
    if (name && std::strcmp(name, world_hills_name) == 0) return world_hills_name;
    if (name && std::strcmp(name, world_cave_name) == 0) return world_cave_name;
    return world_playground_name;
}

static bool is_landscape_world(const char* name) {
    return std::strcmp(canonical_world_name(name), world_hills_name) == 0;
}

static bool is_cave_world(const char* name) {
    return std::strcmp(canonical_world_name(name), world_cave_name) == 0;
}

static int default_quality_radius_for_world(const char* name) {
    return (is_landscape_world(name) || is_cave_world(name)) ? 3 : 4;
}

static int clamped_render_radius(int radius) {
    return static_cast<int>(clampf(static_cast<float>(radius), static_cast<float>(pause_render_radius_min), static_cast<float>(pause_render_radius_max)));
}

template <size_t N>
static int nearest_lighting_option(int value, const int (&options)[N]) {
    int best = options[0];
    int best_distance = std::abs(value - best);
    for (size_t i = 1; i < N; ++i) {
        const int distance = std::abs(value - options[i]);
        if (distance < best_distance) {
            best = options[i];
            best_distance = distance;
        }
    }
    return best;
}

static RadianceCascadeSettings sanitized_lighting_settings(RadianceCascadeSettings settings) {
    constexpr int indirect_options[] = {4, 8, 16, 32, 64};
    constexpr int shadow_options[] = {1, 2, 4, 8, 16};
    constexpr int temporal_options[] = {0, 4, 16, 64, 256};
    settings.probe_extra_levels = static_cast<int>(clampf(static_cast<float>(settings.probe_extra_levels), 0.0f, 2.0f));
    settings.cascade_iterations = static_cast<int>(clampf(static_cast<float>(settings.cascade_iterations), 1.0f, 3.0f));
    settings.indirect_samples = nearest_lighting_option(settings.indirect_samples, indirect_options);
    settings.shadow_samples = nearest_lighting_option(settings.shadow_samples, shadow_options);
    settings.temporal_frames = nearest_lighting_option(settings.temporal_frames, temporal_options);
    settings.lighting_radius_chunks = static_cast<int>(clampf(
        static_cast<float>(settings.lighting_radius_chunks),
        static_cast<float>(pause_lighting_radius_min),
        static_cast<float>(pause_lighting_radius_max)));
    return settings;
}

static void apply_profile_lighting_settings(DemoApp* app) {
    if (!app) return;
    app->profile.lighting = sanitized_lighting_settings(app->profile.lighting);
    app->renderer.lighting = app->profile.lighting;
}

static void copy_world_name(char* dst, size_t cap, const char* src) {
    copy_text(dst, cap, canonical_world_name(src));
}

static void mark_profile_dirty(DemoApp* app) {
    app->profile_dirty = true;
}

static bool parse_profile_bool(const char* value, bool fallback) {
    if (!value) return fallback;
    if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
        std::strcmp(value, "on") == 0 || std::strcmp(value, "yes") == 0) {
        return true;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
        std::strcmp(value, "off") == 0 || std::strcmp(value, "no") == 0) {
        return false;
    }
    return std::atoi(value) != 0;
}

static bool visible_window_ready() {
    return IsWindowReady() && !IsWindowState(FLAG_WINDOW_HIDDEN);
}

static void set_fullscreen_enabled(DemoApp* app, bool enabled, bool persist) {
    if (visible_window_ready() && IsWindowFullscreen() != enabled) {
        ToggleFullscreen();
    }

    const bool stored = visible_window_ready() ? IsWindowFullscreen() : enabled;
    if (app->profile.fullscreen != stored) {
        app->profile.fullscreen = stored;
        if (persist) mark_profile_dirty(app);
    }
}

static void toggle_fullscreen_setting(DemoApp* app) {
    const bool enabled = visible_window_ready() ? !IsWindowFullscreen() : !app->profile.fullscreen;
    set_fullscreen_enabled(app, enabled, true);
}

static void set_fps_counter_enabled(DemoApp* app, bool enabled, bool persist) {
    if (app->profile.show_fps == enabled) return;
    app->profile.show_fps = enabled;
    if (persist) mark_profile_dirty(app);
}

static void set_dimension_render_radius(DemoApp* app, Dimension* dim, int radius, bool persist) {
    if (!dim) return;
    const int clamped = clamped_render_radius(radius);
    if (dim->render_radius_chunks == clamped && (!persist || app->profile.render_radius_chunks == clamped)) return;

    dim->render_radius_chunks = clamped;
    dim->quality_render_radius_chunks = static_cast<i32>(fminf(
        static_cast<float>(default_quality_radius_for_world(app->world_name)),
        static_cast<float>(dim->render_radius_chunks)));
    if (app->landscape_streaming) {
        app->landscape_stream_center_valid = false;
    }
    if (persist) {
        app->profile.render_radius_chunks = clamped;
        mark_profile_dirty(app);
    }
}

static void apply_profile_render_radius(DemoApp* app, Dimension* dim) {
    if (!app || !dim || app->profile.render_radius_chunks <= 0) return;
    set_dimension_render_radius(app, dim, app->profile.render_radius_chunks, false);
}

static u64 local_player_peer_id(const DemoApp* app) {
    return app->net.local_peer_id;
}

static int find_session_index(const DemoProfile& profile, const char* name) {
    if (!name || !name[0]) return -1;
    for (u32 i = 0; i < profile.session_count; ++i) {
        if (profile.sessions[i].valid && std::strcmp(profile.sessions[i].name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static SavedSessionState* find_session(DemoProfile* profile, const char* name) {
    const int idx = find_session_index(*profile, name);
    return idx >= 0 ? &profile->sessions[static_cast<u32>(idx)] : nullptr;
}

static const SavedSessionState* find_session(const DemoProfile* profile, const char* name) {
    const int idx = find_session_index(*profile, name);
    return idx >= 0 ? &profile->sessions[static_cast<u32>(idx)] : nullptr;
}

static SavedSessionState* upsert_session(DemoProfile* profile, const char* name, const char* world_name = nullptr) {
    if (!name || !name[0]) return nullptr;

    const int existing = find_session_index(*profile, name);
    SavedSessionState session{};
    if (existing >= 0) {
        session = profile->sessions[static_cast<u32>(existing)];
        for (int i = existing; i > 0; --i) {
            profile->sessions[static_cast<u32>(i)] = profile->sessions[static_cast<u32>(i - 1)];
        }
    } else {
        if (profile->session_count < max_saved_sessions) {
            profile->session_count++;
        }
        for (u32 i = profile->session_count - 1; i > 0; --i) {
            profile->sessions[i] = profile->sessions[i - 1];
        }
        session = SavedSessionState{};
        session.valid = true;
        copy_text(session.name, sizeof(session.name), name);
        copy_world_name(session.world_name, sizeof(session.world_name), world_name);
    }

    session.valid = true;
    copy_text(session.name, sizeof(session.name), name);
    if (world_name && world_name[0]) {
        copy_world_name(session.world_name, sizeof(session.world_name), world_name);
    } else if (!session.world_name[0]) {
        copy_world_name(session.world_name, sizeof(session.world_name), world_playground_name);
    }
    profile->sessions[0] = session;
    return &profile->sessions[0];
}

static SavedSessionState* append_loaded_session(DemoProfile* profile, const char* name) {
    if (!name || !name[0]) return nullptr;
    if (SavedSessionState* existing = find_session(profile, name)) return existing;
    if (profile->session_count >= max_saved_sessions) return nullptr;

    SavedSessionState& session = profile->sessions[profile->session_count++];
    session = SavedSessionState{};
    session.valid = true;
    copy_text(session.name, sizeof(session.name), name);
    copy_world_name(session.world_name, sizeof(session.world_name), world_playground_name);
    return &session;
}

static void clamp_session_scroll(DemoApp* app) {
    const int max_scroll = app->profile.session_count > max_visible_session_rows
        ? static_cast<int>(app->profile.session_count - max_visible_session_rows)
        : 0;
    if (app->session_scroll < 0) app->session_scroll = 0;
    if (app->session_scroll > max_scroll) app->session_scroll = max_scroll;
}

static void remove_session_at(DemoApp* app, int index) {
    if (index < 0 || static_cast<u32>(index) >= app->profile.session_count) return;
    char removed_name[32]{};
    copy_text(removed_name, sizeof(removed_name), app->profile.sessions[static_cast<u32>(index)].name);

    for (u32 i = static_cast<u32>(index); i + 1 < app->profile.session_count; ++i) {
        app->profile.sessions[i] = app->profile.sessions[i + 1];
    }
    if (app->profile.session_count > 0) {
        app->profile.session_count--;
        app->profile.sessions[app->profile.session_count] = SavedSessionState{};
    }

    if (std::strcmp(app->session_name, removed_name) == 0) {
        const char* replacement = app->profile.session_count > 0 ? app->profile.sessions[0].name : "session";
        copy_text(app->session_name, sizeof(app->session_name), replacement);
        copy_text(app->profile.session_name, sizeof(app->profile.session_name), app->session_name);
        const char* replacement_world = app->profile.session_count > 0 ? app->profile.sessions[0].world_name : app->profile.world_name;
        copy_world_name(app->world_name, sizeof(app->world_name), replacement_world);
        copy_world_name(app->profile.world_name, sizeof(app->profile.world_name), app->world_name);
    }
    app->deleting_session_index = -1;
    app->delete_hold_active = false;
    clamp_session_scroll(app);
    mark_profile_dirty(app);
}

static SavedPlayerState* find_saved_player(SavedSessionState* session, u64 peer_id) {
    if (!session) return nullptr;
    for (u32 i = 0; i < session->player_count; ++i) {
        if (session->players[i].valid && session->players[i].peer_id == peer_id) return &session->players[i];
    }
    return nullptr;
}

static const SavedPlayerState* find_saved_player(const SavedSessionState* session, u64 peer_id) {
    if (!session) return nullptr;
    for (u32 i = 0; i < session->player_count; ++i) {
        if (session->players[i].valid && session->players[i].peer_id == peer_id) return &session->players[i];
    }
    return nullptr;
}

static bool saved_player_state_equal(const SavedPlayerState& a, const SavedPlayerState& b) {
    return a.valid == b.valid &&
        a.peer_id == b.peer_id &&
        std::strcmp(a.name, b.name) == 0 &&
        ColorToInt(a.color) == ColorToInt(b.color) &&
        chunk_equal(a.chunk, b.chunk) &&
        a.local.x == b.local.x && a.local.y == b.local.y && a.local.z == b.local.z &&
        a.yaw == b.yaw && a.pitch == b.pitch &&
        a.body_radius == b.body_radius && a.current_height == b.current_height;
}

static bool upsert_player_state(SavedSessionState* session, const SavedPlayerState& state) {
    if (!session || !state.valid) return false;
    if (SavedPlayerState* existing = find_saved_player(session, state.peer_id)) {
        if (saved_player_state_equal(*existing, state)) return false;
        *existing = state;
        return true;
    }
    if (session->player_count >= max_players) return false;
    session->players[session->player_count++] = state;
    return true;
}

static char hex_digit(unsigned v) {
    return static_cast<char>(v < 10 ? ('0' + v) : ('A' + (v - 10)));
}

static unsigned hex_value(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    return 0;
}

static void encode_text_token(const char* src, char* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) return;
    dst[0] = 0;
    if (!src || !src[0]) {
        if (dst_cap >= 2) std::snprintf(dst, dst_cap, "-");
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src[i] && out + 2 < dst_cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        dst[out++] = hex_digit((c >> 4) & 0x0f);
        dst[out++] = hex_digit(c & 0x0f);
    }
    dst[out] = 0;
}

static void decode_text_token(const char* token, char* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) return;
    dst[0] = 0;
    if (!token || std::strcmp(token, "-") == 0) return;

    size_t out = 0;
    for (size_t i = 0; token[i] && token[i + 1] && out + 1 < dst_cap; i += 2) {
        dst[out++] = static_cast<char>((hex_value(token[i]) << 4) | hex_value(token[i + 1]));
    }
    dst[out] = 0;
}

static void load_profile(DemoApp* app) {
    app->profile = DemoProfile{};

    std::FILE* file = std::fopen("ol_state.txt", "r");
    if (!file) {
        copy_text(app->player_name, sizeof(app->player_name), app->profile.player_name);
        copy_text(app->session_name, sizeof(app->session_name), app->profile.session_name);
        copy_world_name(app->world_name, sizeof(app->world_name), app->profile.world_name);
        app->player_color = app->profile.player_color;
        app->renderer.fov = static_cast<float>(app->profile.fov);
        app->renderer.scale_power = app->profile.scale_power;
        apply_profile_lighting_settings(app);
        return;
    }

    SavedSessionState* current = nullptr;
    int state_version = 1;
    char line[512]{};
    while (std::fgets(line, sizeof(line), file)) {
        char* cmd = std::strtok(line, " \t\r\n");
        if (!cmd || cmd[0] == '#') continue;

        if (std::strcmp(cmd, "OL_STATE") == 0) {
            const char* version = std::strtok(nullptr, " \t\r\n");
            if (version) state_version = std::max(1, std::atoi(version));
        } else if (std::strcmp(cmd, "player") == 0) {
            decode_text_token(std::strtok(nullptr, " \t\r\n"), app->profile.player_name, sizeof(app->profile.player_name));
        } else if (std::strcmp(cmd, "session") == 0) {
            decode_text_token(std::strtok(nullptr, " \t\r\n"), app->profile.session_name, sizeof(app->profile.session_name));
        } else if (std::strcmp(cmd, "world") == 0) {
            char world[32]{};
            decode_text_token(std::strtok(nullptr, " \t\r\n"), world, sizeof(world));
            copy_world_name(app->profile.world_name, sizeof(app->profile.world_name), world);
        } else if (std::strcmp(cmd, "color") == 0) {
            const char* r = std::strtok(nullptr, " \t\r\n");
            const char* g = std::strtok(nullptr, " \t\r\n");
            const char* b = std::strtok(nullptr, " \t\r\n");
            if (r && g && b) {
                app->profile.player_color = {
                    static_cast<unsigned char>(clampf(static_cast<float>(std::atoi(r)), 0.0f, 255.0f)),
                    static_cast<unsigned char>(clampf(static_cast<float>(std::atoi(g)), 0.0f, 255.0f)),
                    static_cast<unsigned char>(clampf(static_cast<float>(std::atoi(b)), 0.0f, 255.0f)),
                    255
                };
            }
        } else if (std::strcmp(cmd, "fov") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.fov = static_cast<int>(clampf(static_cast<float>(std::atoi(value)), 60.0f, 120.0f));
        } else if (std::strcmp(cmd, "scale") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.scale_power = static_cast<int>(clampf(static_cast<float>(std::atoi(value)), 0.0f, 4.0f));
        } else if (std::strcmp(cmd, "render_radius") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.render_radius_chunks = clamped_render_radius(std::atoi(value));
        } else if (std::strcmp(cmd, "lighting_enabled") == 0) {
            app->profile.lighting.enabled = parse_profile_bool(std::strtok(nullptr, " \t\r\n"), app->profile.lighting.enabled);
        } else if (std::strcmp(cmd, "lighting_probe_extra_levels") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.lighting.probe_extra_levels = std::atoi(value);
        } else if (std::strcmp(cmd, "lighting_cascade_iterations") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.lighting.cascade_iterations = std::atoi(value);
        } else if (std::strcmp(cmd, "lighting_indirect_samples") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.lighting.indirect_samples = std::atoi(value);
        } else if (std::strcmp(cmd, "lighting_shadow_samples") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.lighting.shadow_samples = std::atoi(value);
        } else if (std::strcmp(cmd, "lighting_temporal_frames") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.lighting.temporal_frames = std::atoi(value);
        } else if (std::strcmp(cmd, "lighting_radius_chunks") == 0) {
            const char* value = std::strtok(nullptr, " \t\r\n");
            if (value) app->profile.lighting.lighting_radius_chunks = std::atoi(value);
        } else if (std::strcmp(cmd, "lighting_jitter") == 0) {
            app->profile.lighting.jitter = parse_profile_bool(std::strtok(nullptr, " \t\r\n"), app->profile.lighting.jitter);
        } else if (std::strcmp(cmd, "lighting_corner_merge") == 0) {
            app->profile.lighting.corner_merge = parse_profile_bool(std::strtok(nullptr, " \t\r\n"), app->profile.lighting.corner_merge);
        } else if (std::strcmp(cmd, "fullscreen") == 0) {
            app->profile.fullscreen = parse_profile_bool(std::strtok(nullptr, " \t\r\n"), app->profile.fullscreen);
        } else if (std::strcmp(cmd, "fps_counter") == 0) {
            app->profile.show_fps = parse_profile_bool(std::strtok(nullptr, " \t\r\n"), app->profile.show_fps);
        } else if (std::strcmp(cmd, "begin") == 0) {
            char name[32]{};
            decode_text_token(std::strtok(nullptr, " \t\r\n"), name, sizeof(name));
            char world[32]{};
            decode_text_token(std::strtok(nullptr, " \t\r\n"), world, sizeof(world));
            current = name[0] ? append_loaded_session(&app->profile, name) : nullptr;
            if (current) {
                copy_world_name(current->world_name, sizeof(current->world_name), world[0] ? world : world_playground_name);
            }
        } else if (std::strcmp(cmd, "end") == 0) {
            current = nullptr;
        } else if (std::strcmp(cmd, "px") == 0 && current) {
            const char* values[15]{};
            bool complete = true;
            for (const char*& value : values) {
                value = std::strtok(nullptr, " \t\r\n");
                if (!value) complete = false;
            }
            if (!complete || current->painted_pixels.size() >= max_saved_painted_pixels) continue;
            SavedPaintPixel pixel{};
            pixel.chunk = {std::atoi(values[0]), std::atoi(values[1]), std::atoi(values[2])};
            pixel.local = {std::strtof(values[3], nullptr), std::strtof(values[4], nullptr), std::strtof(values[5], nullptr)};
            pixel.normal = {std::strtof(values[6], nullptr), std::strtof(values[7], nullptr), std::strtof(values[8], nullptr)};
            pixel.tangent = {std::strtof(values[9], nullptr), std::strtof(values[10], nullptr), std::strtof(values[11], nullptr)};
            pixel.color = {
                static_cast<u8>(clampf(static_cast<float>(std::atoi(values[12])), 0.0f, 255.0f)),
                static_cast<u8>(clampf(static_cast<float>(std::atoi(values[13])), 0.0f, 255.0f)),
                static_cast<u8>(clampf(static_cast<float>(std::atoi(values[14])), 0.0f, 255.0f)),
                255};
            const char* ou = std::strtok(nullptr, " \t\r\n");
            const char* ov = std::strtok(nullptr, " \t\r\n");
            const char* hu = std::strtok(nullptr, " \t\r\n");
            const char* hv = std::strtok(nullptr, " \t\r\n");
            const char* sprite_id = std::strtok(nullptr, " \t\r\n");
            const char* sprite_x = std::strtok(nullptr, " \t\r\n");
            const char* sprite_y = std::strtok(nullptr, " \t\r\n");
            const char* mesh_id = std::strtok(nullptr, " \t\r\n");
            if (ou && ov && hu && hv) {
                pixel.quad_offset = {std::strtof(ou, nullptr), std::strtof(ov, nullptr)};
                pixel.quad_half_size = {std::strtof(hu, nullptr), std::strtof(hv, nullptr)};
            }
            if (sprite_id && sprite_x && sprite_y) {
                pixel.sprite_id = static_cast<u32>(std::strtoul(sprite_id, nullptr, 10));
                pixel.sprite_pixel_x = std::atoi(sprite_x);
                pixel.sprite_pixel_y = std::atoi(sprite_y);
            }
            if (mesh_id) pixel.mesh_id = static_cast<u32>(std::strtoul(mesh_id, nullptr, 10));
            if (state_version < 2 && !id_valid(pixel.sprite_id)) {
                WorldPos old_center{0, pixel.chunk, pixel.local};
                worldpos_add_delta(&old_center, pixel.normal * -0.006f, 16.0f);
                pixel.chunk = old_center.chunk;
                pixel.local = old_center.local;
            }
            current->painted_pixels.push_back(pixel);
        } else if (std::strcmp(cmd, "p") == 0 && current) {
            const char* peer = std::strtok(nullptr, " \t\r\n");
            const char* cx = std::strtok(nullptr, " \t\r\n");
            const char* cy = std::strtok(nullptr, " \t\r\n");
            const char* cz = std::strtok(nullptr, " \t\r\n");
            const char* lx = std::strtok(nullptr, " \t\r\n");
            const char* ly = std::strtok(nullptr, " \t\r\n");
            const char* lz = std::strtok(nullptr, " \t\r\n");
            const char* yaw = std::strtok(nullptr, " \t\r\n");
            const char* pitch = std::strtok(nullptr, " \t\r\n");
            const char* radius = std::strtok(nullptr, " \t\r\n");
            const char* height = std::strtok(nullptr, " \t\r\n");
            const char* cr = std::strtok(nullptr, " \t\r\n");
            const char* cg = std::strtok(nullptr, " \t\r\n");
            const char* cb = std::strtok(nullptr, " \t\r\n");
            const char* name = std::strtok(nullptr, " \t\r\n");
            if (!peer || !cx || !cy || !cz || !lx || !ly || !lz || !yaw || !pitch || !radius || !height || !cr || !cg || !cb) continue;

            SavedPlayerState state{};
            state.valid = true;
            state.peer_id = static_cast<u64>(std::strtoull(peer, nullptr, 10));
            state.chunk = {std::atoi(cx), std::atoi(cy), std::atoi(cz)};
            state.local = {std::strtof(lx, nullptr), std::strtof(ly, nullptr), std::strtof(lz, nullptr)};
            state.yaw = std::strtof(yaw, nullptr);
            state.pitch = std::strtof(pitch, nullptr);
            state.body_radius = std::strtof(radius, nullptr);
            state.current_height = std::strtof(height, nullptr);
            state.color = {
                static_cast<unsigned char>(clampf(static_cast<float>(std::atoi(cr)), 0.0f, 255.0f)),
                static_cast<unsigned char>(clampf(static_cast<float>(std::atoi(cg)), 0.0f, 255.0f)),
                static_cast<unsigned char>(clampf(static_cast<float>(std::atoi(cb)), 0.0f, 255.0f)),
                255
            };
            decode_text_token(name, state.name, sizeof(state.name));
            upsert_player_state(current, state);
        }
    }
    std::fclose(file);

    copy_text(app->player_name, sizeof(app->player_name), app->profile.player_name);
    copy_text(app->session_name, sizeof(app->session_name), app->profile.session_name);
    copy_world_name(app->world_name, sizeof(app->world_name), app->profile.world_name);
    app->player_color = app->profile.player_color;
    app->renderer.fov = static_cast<float>(app->profile.fov);
    app->renderer.scale_power = app->profile.scale_power;
    apply_profile_lighting_settings(app);
    app->profile_dirty = false;
}

static void sync_profile_from_app(DemoApp* app) {
    copy_text(app->profile.player_name, sizeof(app->profile.player_name), app->player_name);
    copy_text(app->profile.session_name, sizeof(app->profile.session_name), app->session_name);
    copy_world_name(app->profile.world_name, sizeof(app->profile.world_name), app->world_name);
    app->profile.player_color = app->player_color;
    app->profile.fov = static_cast<int>(clampf(floorf(app->renderer.fov + 0.5f), 60.0f, 120.0f));
    app->profile.scale_power = static_cast<int>(clampf(static_cast<float>(app->renderer.scale_power), 0.0f, 4.0f));
    app->profile.lighting = sanitized_lighting_settings(app->renderer.lighting);
    if (visible_window_ready()) {
        app->profile.fullscreen = IsWindowFullscreen();
    }
    if (app->profile.render_radius_chunks > 0) {
        app->profile.render_radius_chunks = clamped_render_radius(app->profile.render_radius_chunks);
    }
}

static bool write_profile_snapshot(const DemoProfile& profile) {
    std::FILE* file = std::fopen("ol_state.txt", "w");
    if (!file) return false;

    char encoded[128]{};
    std::fprintf(file, "OL_STATE 2\n");
    encode_text_token(profile.player_name, encoded, sizeof(encoded));
    std::fprintf(file, "player %s\n", encoded);
    encode_text_token(profile.session_name, encoded, sizeof(encoded));
    std::fprintf(file, "session %s\n", encoded);
    encode_text_token(profile.world_name, encoded, sizeof(encoded));
    std::fprintf(file, "world %s\n", encoded);
    std::fprintf(file, "color %u %u %u\n",
        static_cast<unsigned>(profile.player_color.r),
        static_cast<unsigned>(profile.player_color.g),
        static_cast<unsigned>(profile.player_color.b));
    std::fprintf(file, "fov %d\n", profile.fov);
    std::fprintf(file, "scale %d\n", profile.scale_power);
    std::fprintf(file, "fullscreen %d\n", profile.fullscreen ? 1 : 0);
    std::fprintf(file, "fps_counter %d\n", profile.show_fps ? 1 : 0);
    std::fprintf(file, "lighting_enabled %d\n", profile.lighting.enabled ? 1 : 0);
    std::fprintf(file, "lighting_probe_extra_levels %d\n", profile.lighting.probe_extra_levels);
    std::fprintf(file, "lighting_cascade_iterations %d\n", profile.lighting.cascade_iterations);
    std::fprintf(file, "lighting_indirect_samples %d\n", profile.lighting.indirect_samples);
    std::fprintf(file, "lighting_shadow_samples %d\n", profile.lighting.shadow_samples);
    std::fprintf(file, "lighting_temporal_frames %d\n", profile.lighting.temporal_frames);
    std::fprintf(file, "lighting_radius_chunks %d\n", profile.lighting.lighting_radius_chunks);
    std::fprintf(file, "lighting_jitter %d\n", profile.lighting.jitter ? 1 : 0);
    std::fprintf(file, "lighting_corner_merge %d\n", profile.lighting.corner_merge ? 1 : 0);
    if (profile.render_radius_chunks > 0) {
        std::fprintf(file, "render_radius %d\n", profile.render_radius_chunks);
    }

    for (u32 i = 0; i < profile.session_count; ++i) {
        const SavedSessionState& session = profile.sessions[i];
        if (!session.valid || !session.name[0]) continue;
        encode_text_token(session.name, encoded, sizeof(encoded));
        char encoded_world[128]{};
        encode_text_token(session.world_name[0] ? session.world_name : world_playground_name, encoded_world, sizeof(encoded_world));
        std::fprintf(file, "begin %s %s\n", encoded, encoded_world);
        for (u32 p = 0; p < session.player_count; ++p) {
            const SavedPlayerState& player = session.players[p];
            if (!player.valid) continue;
            char encoded_name[128]{};
            encode_text_token(player.name, encoded_name, sizeof(encoded_name));
            std::fprintf(
                file,
                "p %llu %d %d %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %u %u %u %s\n",
                static_cast<unsigned long long>(player.peer_id),
                player.chunk.x,
                player.chunk.y,
                player.chunk.z,
                player.local.x,
                player.local.y,
                player.local.z,
                player.yaw,
                player.pitch,
                player.body_radius,
                player.current_height,
                static_cast<unsigned>(player.color.r),
                static_cast<unsigned>(player.color.g),
                static_cast<unsigned>(player.color.b),
                encoded_name);
        }
        for (const SavedPaintPixel& pixel : session.painted_pixels) {
            std::fprintf(
                file,
                "px %d %d %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %u %u %u %.6f %.6f %.6f %.6f %u %d %d %u\n",
                pixel.chunk.x, pixel.chunk.y, pixel.chunk.z,
                pixel.local.x, pixel.local.y, pixel.local.z,
                pixel.normal.x, pixel.normal.y, pixel.normal.z,
                pixel.tangent.x, pixel.tangent.y, pixel.tangent.z,
                static_cast<unsigned>(pixel.color.r),
                static_cast<unsigned>(pixel.color.g),
                static_cast<unsigned>(pixel.color.b),
                pixel.quad_offset.x, pixel.quad_offset.y,
                pixel.quad_half_size.x, pixel.quad_half_size.y,
                pixel.sprite_id, pixel.sprite_pixel_x, pixel.sprite_pixel_y, pixel.mesh_id);
        }
        std::fprintf(file, "end\n");
    }

    bool write_failed = std::ferror(file) != 0;
    if (std::fflush(file) != 0) write_failed = true;
    const bool close_failed = std::fclose(file) != 0;
    return !write_failed && !close_failed;
}

struct ProfileSaveWorker {
    std::mutex mutex{};
    std::condition_variable wake{};
    std::thread thread{};
    bool stop = false;
    bool pending = false;
    bool writing = false;
    bool write_failed = false;
    DemoProfile pending_profile{};
};

static ProfileSaveWorker* profile_save_worker(DemoApp* app) {
    return app ? static_cast<ProfileSaveWorker*>(app->profile_save_worker) : nullptr;
}

static void profile_save_worker_main(ProfileSaveWorker* state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    for (;;) {
        state->wake.wait(lock, [&]() { return state->stop || state->pending; });
        if (state->stop && !state->pending) break;

        DemoProfile snapshot = std::move(state->pending_profile);
        state->pending = false;
        state->writing = true;
        lock.unlock();
        const bool wrote = write_profile_snapshot(snapshot);
        snapshot = DemoProfile{};
        lock.lock();
        state->writing = false;
        if (!wrote) state->write_failed = true;
        state->wake.notify_all();
    }
}

static void start_profile_save_worker(DemoApp* app) {
    if (!app || app->profile_save_worker) return;
    auto* state = new ProfileSaveWorker{};
    state->thread = std::thread(profile_save_worker_main, state);
    app->profile_save_worker = state;
}

static void collect_profile_save_failure(DemoApp* app) {
    ProfileSaveWorker* state = profile_save_worker(app);
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->write_failed) return;
    state->write_failed = false;
    app->profile_dirty = true;
}

static void flush_profile_save_worker(DemoApp* app) {
    ProfileSaveWorker* state = profile_save_worker(app);
    if (!state) return;
    std::unique_lock<std::mutex> lock(state->mutex);
    state->wake.wait(lock, [&]() { return !state->pending && !state->writing; });
    if (state->write_failed) {
        state->write_failed = false;
        app->profile_dirty = true;
    }
}

static void stop_profile_save_worker(DemoApp* app) {
    ProfileSaveWorker* state = profile_save_worker(app);
    if (!state) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stop = true;
    }
    state->wake.notify_all();
    if (state->thread.joinable()) state->thread.join();
    if (state->write_failed) app->profile_dirty = true;
    delete state;
    app->profile_save_worker = nullptr;
}

static void queue_profile_save(DemoApp* app) {
    if (!app) return;
    sync_profile_from_app(app);
    start_profile_save_worker(app);
    DemoProfile snapshot = app->profile;
    ProfileSaveWorker* state = profile_save_worker(app);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pending_profile = std::move(snapshot);
        state->pending = true;
    }
    state->wake.notify_one();
    app->profile_dirty = false;
    app->last_profile_save_time = GetTime();
    ++app->debug_profile_autosaves_queued;
}

static void save_profile(DemoApp* app) {
    if (!app) return;
    sync_profile_from_app(app);
    flush_profile_save_worker(app);
    if (!write_profile_snapshot(app->profile)) {
        app->profile_dirty = true;
        return;
    }
    app->profile_dirty = false;
    app->last_profile_save_time = GetTime();
    ++app->debug_profile_sync_saves;
}

static WorldPos demo_pos(u32 dimension, Vector3 meters, float chunk_size) {
    return make_world_pos(dimension, meters, chunk_size);
}

static u32 hash_u32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

static u32 hash_coords(i32 x, i32 z, u32 seed) {
    u32 h = static_cast<u32>(x) * 0x9e3779b9u;
    h ^= static_cast<u32>(z) * 0x85ebca6bu;
    h ^= seed * 0xc2b2ae35u;
    return hash_u32(h);
}

static float hash01(i32 x, i32 z, u32 seed) {
    return static_cast<float>(hash_coords(x, z, seed) & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

static float smoothstep01(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float lerp_float(float a, float b, float t) {
    return a + (b - a) * t;
}

static float value_noise(float x, float z, float frequency, u32 seed) {
    const float sx = x * frequency;
    const float sz = z * frequency;
    const i32 ix = static_cast<i32>(std::floor(sx));
    const i32 iz = static_cast<i32>(std::floor(sz));
    const float fx = smoothstep01(sx - static_cast<float>(ix));
    const float fz = smoothstep01(sz - static_cast<float>(iz));

    const float a = hash01(ix, iz, seed);
    const float b = hash01(ix + 1, iz, seed);
    const float c = hash01(ix, iz + 1, seed);
    const float d = hash01(ix + 1, iz + 1, seed);
    return lerp_float(lerp_float(a, b, fx), lerp_float(c, d, fx), fz);
}

static float landscape_raw_height(float x, float z) {
    const float broad = value_noise(x, z, 0.012f, 11) * 2.0f - 1.0f;
    const float hills = value_noise(x, z, 0.032f, 23) * 2.0f - 1.0f;
    const float detail = value_noise(x, z, 0.085f, 37) * 2.0f - 1.0f;
    return broad * 2.6f + hills * 1.35f + detail * 0.35f;
}

static float landscape_height(float x, float z) {
    const float spawn_bias = landscape_raw_height(0.0f, 8.0f);
    return clampf(landscape_raw_height(x, z) - spawn_bias, -0.7f, 5.4f);
}

static float landscape_tile_height(float x, float z, float chunk_size) {
    const float tile = chunk_size * 0.5f;
    const float tx = std::floor(x / tile) * tile + tile * 0.5f;
    const float tz = std::floor(z / tile) * tile + tile * 0.5f;
    return landscape_height(tx, tz);
}

static WorldPos default_player_spawn(const DemoApp* app, u32 dimension_id, float chunk_size) {
    if (app && is_landscape_world(app->world_name)) {
        const float y = landscape_tile_height(0.0f, 8.0f, chunk_size) + 0.08f;
        return demo_pos(dimension_id, {0.0f, y, 8.0f}, chunk_size);
    }
    if (app && is_cave_world(app->world_name)) {
        return demo_pos(dimension_id, {8.0f, 0.0f, 27.0f}, chunk_size);
    }
    return demo_pos(dimension_id, {0.0f, 0.0f, 8.0f}, chunk_size);
}

static void reset_remote_players(DemoApp* app) {
    app->remote_peer_ids.fill(0);
    app->remote_player_ids.fill(invalid_id);
    app->paint_sync_sent_peer_ids.fill(0);
}

static void write_smoke_log(const char* fmt, ...) {
    char message[256]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    std::printf("%s\n", message);
    std::fflush(stdout);

    if (std::FILE* file = std::fopen("steam-smoke.log", "w")) {
        std::fprintf(file, "%s\n", message);
        std::fclose(file);
    }
}

static void write_smoke_lobby_file(u64 lobby_id) {
    if (std::FILE* file = std::fopen("steam-smoke-lobby.txt", "w")) {
        std::fprintf(file, "%llu\n", static_cast<unsigned long long>(lobby_id));
        std::fclose(file);
    }
}

static u32 add_visual_box(Dimension* dim, u32 dimension_id, u32 cube_geom, const char* name, Vector3 center, Vector3 size, Color color) {
    MeshInstance mesh{};
    std::snprintf(mesh.name, sizeof(mesh.name), "%s", name);
    mesh.geometry = cube_geom;
    mesh.origin = demo_pos(dimension_id, center, dim->chunk_size_m);
    mesh.se3 = MatrixScale(size.x, size.y, size.z);
    mesh.color = color;
    mesh.texture_id = render_texture_grid;
    mesh.bounds_radius = Vector3Length(size) * 0.5f;
    return dimension_add_mesh(dim, mesh);
}

static void add_box_collider(Dimension* dim, u32 dimension_id, Vector3 center, Vector3 size, Color color) {
    BoxCollider box{};
    box.pos = demo_pos(dimension_id, center, dim->chunk_size_m);
    box.half = size * 0.5f;
    box.color = color;
    physics_add_box(&dim->physics, box);
}

static void add_static_box(Dimension* dim, u32 dimension_id, u32 cube_geom, const char* name, Vector3 center, Vector3 size, Color color, bool lit = true, bool draw_edges = true) {
    const u32 mesh_id = add_visual_box(dim, dimension_id, cube_geom, name, center, size, color);
    MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
    if (mesh) {
        mesh->lit = lit;
        mesh->draw_edges = draw_edges;
    }

    add_box_collider(dim, dimension_id, center, size, color);
}

static void push_emissive_material(MeshInstance* mesh, const char* name, Color color, float emission) {
    if (!mesh) return;
    MaterialLayer layer{};
    std::snprintf(layer.name, sizeof(layer.name), "%s", name);
    layer.has_emission = true;
    layer.emission_color = color;
    layer.emission = emission;
    material_stack_push(&mesh->materials, layer);
}

static void push_reflective_material(MeshInstance* mesh, const char* name, float reflectivity) {
    if (!mesh) return;
    MaterialLayer layer{};
    std::snprintf(layer.name, sizeof(layer.name), "%s", name);
    layer.has_reflectivity = true;
    layer.reflectivity = reflectivity;
    material_stack_push(&mesh->materials, layer);
}

static u32 add_emissive_box(
    Dimension* dim,
    u32 dimension_id,
    u32 cube_geom,
    const char* name,
    Vector3 center,
    Vector3 size,
    Color color,
    float emission
) {
    const u32 mesh_id = add_visual_box(dim, dimension_id, cube_geom, name, center, size, color);
    if (MeshInstance* mesh = arena_get(&dim->meshes, mesh_id)) {
        mesh->lit = false;
        mesh->draw_edges = false;
        push_emissive_material(mesh, "emission", color, emission);
    }
    return mesh_id;
}

static u32 add_cave_box(
    Dimension* dim,
    u32 dimension_id,
    u32 cube_geom,
    const char* name,
    Vector3 center,
    Vector3 size,
    Color color,
    bool collider,
    float reflectivity = 0.0f,
    bool draw_edges = true
) {
    const u32 mesh_id = add_visual_box(dim, dimension_id, cube_geom, name, center, size, color);
    if (MeshInstance* mesh = arena_get(&dim->meshes, mesh_id)) {
        mesh->draw_edges = draw_edges;
        if (reflectivity > 0.0f) push_reflective_material(mesh, "cave surface", reflectivity);
    }
    if (collider) add_box_collider(dim, dimension_id, center, size, color);
    return mesh_id;
}

static void reset_cave_animation(DemoApp* app) {
    if (!app) return;
    app->cave_emissive_mesh_ids.fill(invalid_id);
    app->cave_emissive_base_positions.fill({});
    app->cave_animation_time = 0.0f;
}

static void clear_streamed_chunk_records(DemoApp* app) {
    app->streamed_chunks.clear();
    app->streamed_chunks.reserve(512);
    app->landscape_stream_center_valid = false;
    app->debug_landscape_chunks_loaded = 0;
    app->debug_landscape_chunks_unloaded = 0;
}

static bool streamed_chunk_matches(const StreamedWorldChunk& chunk, ChunkCoord coord) {
    return chunk.valid && chunk.coord.x == coord.x && chunk.coord.z == coord.z;
}

static StreamedWorldChunk* find_streamed_chunk(DemoApp* app, ChunkCoord coord) {
    for (StreamedWorldChunk& chunk : app->streamed_chunks) {
        if (streamed_chunk_matches(chunk, coord)) return &chunk;
    }
    return nullptr;
}

static StreamedWorldChunk* acquire_streamed_chunk_record(DemoApp* app, ChunkCoord coord) {
    if (StreamedWorldChunk* existing = find_streamed_chunk(app, coord)) return existing;
    for (StreamedWorldChunk& chunk : app->streamed_chunks) {
        if (chunk.valid) continue;
        chunk = StreamedWorldChunk{};
        chunk.valid = true;
        chunk.coord = coord;
        return &chunk;
    }
    app->streamed_chunks.push_back(StreamedWorldChunk{});
    StreamedWorldChunk& chunk = app->streamed_chunks.back();
    chunk.valid = true;
    chunk.coord = coord;
    return &chunk;
}

static void record_streamed_mesh(StreamedWorldChunk* chunk, u32 mesh_id) {
    if (!chunk || !id_valid(mesh_id) || chunk->mesh_count >= max_streamed_chunk_meshes) return;
    chunk->meshes[chunk->mesh_count++] = mesh_id;
}

static void record_streamed_box(StreamedWorldChunk* chunk, u32 box_id) {
    if (!chunk || !id_valid(box_id) || chunk->box_count >= max_streamed_chunk_boxes) return;
    chunk->boxes[chunk->box_count++] = box_id;
}

static void unload_streamed_chunk_colliders(Dimension* dim, StreamedWorldChunk* chunk) {
    if (!dim || !chunk || !chunk->valid) return;
    for (u32 i = 0; i < chunk->box_count; ++i) {
        physics_remove_box(&dim->physics, chunk->boxes[i]);
        chunk->boxes[i] = invalid_id;
    }
    chunk->box_count = 0;
    chunk->colliders_loaded = false;
}

static void unload_streamed_chunk(Dimension* dim, StreamedWorldChunk* chunk) {
    if (!dim || !chunk || !chunk->valid) return;
    for (u32 i = 0; i < chunk->mesh_count; ++i) {
        dimension_remove_mesh(dim, chunk->meshes[i]);
    }
    unload_streamed_chunk_colliders(dim, chunk);
    *chunk = StreamedWorldChunk{};
}

static Color landscape_ground_color(float height, float noise) {
    if (height > 3.8f) return {118, 132, 128, 255};
    if (height > 2.2f) return {112, 144, 105, 255};
    if (height < -0.15f) return {96, 132, 126, 255};
    const unsigned char r = static_cast<unsigned char>(clampf(92.0f + noise * 28.0f, 0.0f, 255.0f));
    const unsigned char g = static_cast<unsigned char>(clampf(132.0f + noise * 34.0f, 0.0f, 255.0f));
    const unsigned char b = static_cast<unsigned char>(clampf(86.0f + noise * 20.0f, 0.0f, 255.0f));
    return {r, g, b, 255};
}

static void add_streamed_box(DemoApp* app, Dimension* dim, StreamedWorldChunk* chunk, const char* name, Vector3 center, Vector3 size, Color color, bool collider, bool lit, bool draw_edges) {
    if (!app || !dim || !chunk) return;

    const u32 mesh_id = add_visual_box(dim, app->dimension_id, app->landscape_cube_geom, name, center, size, color);
    if (MeshInstance* mesh = arena_get(&dim->meshes, mesh_id)) {
        mesh->lit = lit;
        mesh->draw_edges = draw_edges;
        if (std::strstr(name, "terrain")) mesh->texture_id = render_texture_grass;
        else if (std::strstr(name, "roof")) mesh->texture_id = render_texture_roof;
        else if (std::strstr(name, "wall") || std::strstr(name, "floor")) mesh->texture_id = render_texture_stone;
        else mesh->texture_id = render_texture_grid;
    }
    record_streamed_mesh(chunk, mesh_id);

    if (!collider) return;
    BoxCollider box{};
    box.pos = demo_pos(app->dimension_id, center, dim->chunk_size_m);
    box.half = size * 0.5f;
    box.color = color;
    record_streamed_box(chunk, physics_add_box(&dim->physics, box));
}

static void add_landscape_building(DemoApp* app, Dimension* dim, StreamedWorldChunk* chunk, ChunkCoord coord, bool collider) {
    const float chunk_size = dim->chunk_size_m;
    const float base_x = static_cast<float>(coord.x) * chunk_size + chunk_size * 0.5f;
    const float base_z = static_cast<float>(coord.z) * chunk_size + chunk_size * 0.5f;
    const float base_y = landscape_tile_height(base_x, base_z, chunk_size) + 0.08f;
    const u32 variant = hash_coords(coord.x, coord.z, 221) % 3u;

    const Color wall = variant == 0 ? Color{164, 156, 128, 255} : (variant == 1 ? Color{122, 145, 164, 255} : Color{151, 126, 160, 255});
    const Color roof = variant == 0 ? Color{93, 103, 118, 255} : (variant == 1 ? Color{92, 87, 104, 255} : Color{116, 90, 96, 255});
    const Color floor = {132, 120, 94, 255};

    add_streamed_box(app, dim, chunk, "hill house floor", {base_x, base_y - 0.10f, base_z}, {8.4f, 0.32f, 8.4f}, floor, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house back wall", {base_x, base_y + 1.45f, base_z - 4.05f}, {7.70f, 2.9f, 0.35f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house left wall", {base_x - 4.05f, base_y + 1.45f, base_z}, {0.35f, 2.9f, 8.4f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house right wall", {base_x + 4.05f, base_y + 1.45f, base_z}, {0.35f, 2.9f, 8.4f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house front left", {base_x - 2.6625f, base_y + 1.45f, base_z + 4.05f}, {2.425f, 2.9f, 0.35f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house front right", {base_x + 2.6625f, base_y + 1.45f, base_z + 4.05f}, {2.425f, 2.9f, 0.35f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house lintel", {base_x, base_y + 2.55f, base_z + 4.05f}, {2.9f, 0.70f, 0.35f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house roof", {base_x, base_y + 3.10f, base_z}, {8.9f, 0.45f, 8.9f}, roof, collider, true, true);

    if (variant == 0) {
        add_streamed_box(app, dim, chunk, "interior table", {base_x - 1.2f, base_y + 0.45f, base_z - 0.8f}, {2.3f, 0.55f, 1.2f}, {112, 82, 58, 255}, collider, true, true);
        add_streamed_box(app, dim, chunk, "interior shelf", {base_x + 3.05f, base_y + 1.0f, base_z - 1.8f}, {0.45f, 1.8f, 3.0f}, {92, 72, 56, 255}, collider, true, true);
        add_streamed_box(app, dim, chunk, "interior crate", {base_x + 1.5f, base_y + 0.35f, base_z + 1.8f}, {0.9f, 0.7f, 0.9f}, {154, 111, 67, 255}, collider, true, true);
    } else if (variant == 1) {
        add_streamed_box(app, dim, chunk, "interior plinth", {base_x, base_y + 0.65f, base_z - 0.4f}, {1.2f, 1.1f, 1.2f}, {88, 116, 142, 255}, collider, true, true);
        add_streamed_box(app, dim, chunk, "interior bench a", {base_x - 2.2f, base_y + 0.28f, base_z + 1.7f}, {2.6f, 0.35f, 0.7f}, {112, 96, 78, 255}, collider, true, true);
        add_streamed_box(app, dim, chunk, "interior bench b", {base_x + 2.2f, base_y + 0.28f, base_z + 1.7f}, {2.6f, 0.35f, 0.7f}, {112, 96, 78, 255}, collider, true, true);
    } else {
        add_streamed_box(app, dim, chunk, "interior pillar a", {base_x - 2.4f, base_y + 1.15f, base_z - 2.0f}, {0.45f, 2.3f, 0.45f}, {118, 96, 132, 255}, collider, true, true);
        add_streamed_box(app, dim, chunk, "interior pillar b", {base_x + 2.4f, base_y + 1.15f, base_z - 2.0f}, {0.45f, 2.3f, 0.45f}, {118, 96, 132, 255}, collider, true, true);
        add_streamed_box(app, dim, chunk, "interior low wall", {base_x, base_y + 0.45f, base_z + 0.7f}, {3.4f, 0.75f, 0.45f}, {102, 84, 118, 255}, collider, true, true);
    }
}

static void generate_landscape_chunk(DemoApp* app, Dimension* dim, ChunkCoord coord, bool collidable) {
    StreamedWorldChunk* chunk = acquire_streamed_chunk_record(app, coord);
    if (!chunk || chunk->mesh_count > 0 || chunk->box_count > 0) return;
    chunk->colliders_loaded = collidable;

    const float chunk_size = dim->chunk_size_m;
    const float tile = chunk_size * 0.5f;
    const float min_x = static_cast<float>(coord.x) * chunk_size;
    const float min_z = static_cast<float>(coord.z) * chunk_size;
    constexpr float terrain_bottom = -2.4f;

    for (u32 z = 0; z < 2; ++z) {
        for (u32 x = 0; x < 2; ++x) {
            const float cx = min_x + (static_cast<float>(x) + 0.5f) * tile;
            const float cz = min_z + (static_cast<float>(z) + 0.5f) * tile;
            const float top = landscape_height(cx, cz);
            const float h = fmaxf(0.35f, top - terrain_bottom);
            const float n = value_noise(cx, cz, 0.11f, 331);
            add_streamed_box(
                app,
                dim,
                chunk,
                "landscape terrain",
                {cx, terrain_bottom + h * 0.5f, cz},
                {tile + 0.08f, h, tile + 0.08f},
                landscape_ground_color(top, n),
                collidable,
                true,
                false);
        }
    }

    const i32 near_spawn = std::abs(coord.x) + std::abs(coord.z);
    const bool has_building = near_spawn > 2 && hash01(coord.x, coord.z, 503) > 0.875f;
    if (has_building) {
        const float center_x = min_x + chunk_size * 0.5f;
        const float center_z = min_z + chunk_size * 0.5f;
        if (landscape_tile_height(center_x, center_z, chunk_size) > 0.35f) {
            add_landscape_building(app, dim, chunk, coord, collidable);
        }
    }
}

static void ensure_landscape_chunks_around(DemoApp* app, Dimension* dim, WorldPos center) {
    if (!app || !dim || !app->landscape_streaming) return;
    const bool initial_fill = !app->landscape_stream_center_valid;
    app->landscape_stream_center_valid = true;
    app->landscape_stream_center = center.chunk;
    app->debug_landscape_chunks_loaded = 0;
    app->debug_landscape_chunks_unloaded = 0;

    const i32 radius = dim->render_radius_chunks + 1;
    const i32 keep_radius = radius + 1;
    constexpr i32 landscape_collider_radius_chunks = 3;
    const i32 collider_radius = radius < landscape_collider_radius_chunks ? radius : landscape_collider_radius_chunks;
    const i32 collider_radius_sq = collider_radius * collider_radius;
    const i32 cx = center.chunk.x;
    const i32 cz = center.chunk.z;
    const i32 keep_diameter = keep_radius * 2 + 1;
    const u32 reserve_count = static_cast<u32>(keep_diameter * keep_diameter);
    if (app->streamed_chunks.capacity() < reserve_count) {
        app->streamed_chunks.reserve(reserve_count);
    }

    constexpr u32 streamed_chunk_commit_budget = 2;
    const u32 unload_budget = initial_fill ? std::numeric_limits<u32>::max() : streamed_chunk_commit_budget;
    for (StreamedWorldChunk& chunk : app->streamed_chunks) {
        if (!chunk.valid) continue;
        const i32 dx = chunk.coord.x - cx;
        const i32 dz = chunk.coord.z - cz;
        const i32 dist_sq = dx * dx + dz * dz;
        if (dist_sq > keep_radius * keep_radius) {
            if (app->debug_landscape_chunks_unloaded >= unload_budget) continue;
            unload_streamed_chunk(dim, &chunk);
            ++app->debug_landscape_chunks_unloaded;
        } else if (dist_sq > collider_radius_sq && chunk.colliders_loaded) {
            if (app->debug_landscape_chunks_unloaded >= unload_budget) continue;
            unload_streamed_chunk_colliders(dim, &chunk);
            ++app->debug_landscape_chunks_unloaded;
        }
    }

    struct PendingChunk {
        ChunkCoord coord{};
        i32 distance_sq = 0;
        bool collidable = false;
        bool rebuild = false;
    };
    std::vector<PendingChunk> pending{};
    pending.reserve(static_cast<size_t>((radius * 2 + 1) * (radius * 2 + 1)));
    for (i32 dz = -radius; dz <= radius; ++dz) {
        for (i32 dx = -radius; dx <= radius; ++dx) {
            const i32 distance_sq = dx * dx + dz * dz;
            if (distance_sq > radius * radius) continue;
            const ChunkCoord coord{cx + dx, 0, cz + dz};
            const bool collidable = distance_sq <= collider_radius_sq;
            StreamedWorldChunk* chunk = find_streamed_chunk(app, coord);
            if (!chunk || (collidable && !chunk->colliders_loaded)) {
                pending.push_back({coord, distance_sq, collidable, chunk != nullptr});
            }
        }
    }
    std::stable_sort(pending.begin(), pending.end(), [](const PendingChunk& a, const PendingChunk& b) {
        if (a.collidable != b.collidable) return a.collidable;
        if (a.distance_sq != b.distance_sq) return a.distance_sq < b.distance_sq;
        if (a.coord.z != b.coord.z) return a.coord.z < b.coord.z;
        return a.coord.x < b.coord.x;
    });
    const u32 load_budget = initial_fill ? std::numeric_limits<u32>::max() : streamed_chunk_commit_budget;
    for (const PendingChunk& request : pending) {
        if (app->debug_landscape_chunks_loaded >= load_budget) break;
        if (request.rebuild) {
            if (StreamedWorldChunk* chunk = find_streamed_chunk(app, request.coord)) {
                unload_streamed_chunk(dim, chunk);
            }
        }
        generate_landscape_chunk(app, dim, request.coord, request.collidable);
        ++app->debug_landscape_chunks_loaded;
    }
}

void demo_update_landscape_streaming(DemoApp* app, Dimension* dim, const PlayerEntity* player) {
    if (!app || !dim || !player || !app->landscape_streaming) return;
    WorldPos center = player_feet_pos(dim, player);
    constexpr float prefetch_margin_m = 5.0f;
    if (center.local.x < prefetch_margin_m && player->velocity.x < -0.1f) --center.chunk.x;
    if (center.local.x > dim->chunk_size_m - prefetch_margin_m && player->velocity.x > 0.1f) ++center.chunk.x;
    if (center.local.z < prefetch_margin_m && player->velocity.z < -0.1f) --center.chunk.z;
    if (center.local.z > dim->chunk_size_m - prefetch_margin_m && player->velocity.z > 0.1f) ++center.chunk.z;
    center.local = {};
    ensure_landscape_chunks_around(app, dim, center);
}

static u16 add_contour_vertex(MeshGeometry* geometry, Vector3 vertex) {
    if (geometry->vertex_count >= max_vertices_per_geometry) return 0;
    const u16 index = static_cast<u16>(geometry->vertex_count++);
    geometry->vertices[index] = vertex;
    return index;
}

static void add_contour_edge(MeshGeometry* geometry, u16 a, u16 b, u8 thickness_px = 2) {
    if (geometry->edge_count >= max_edges_per_geometry) return;
    geometry->edges[geometry->edge_count++] = {a, b, thickness_px};
}

static MeshGeometry make_stair_contour_geometry(const char* name) {
    MeshGeometry geometry{};
    std::snprintf(geometry.name, sizeof(geometry.name), "%s", name);

    constexpr float xh = 1.5f;
    constexpr float z0 = -4.5f;
    constexpr float z1 = -1.5f;
    constexpr float z2 = 1.5f;
    constexpr float z3 = 4.5f;
    constexpr float y0 = -0.5f;
    constexpr float y1 = 0.5f;
    constexpr float y2 = 1.5f;
    constexpr float y3 = 2.5f;

    const Vector3 left[] = {
        {-xh, y0, z0}, {-xh, y0, z3}, {-xh, y3, z3}, {-xh, y3, z2},
        {-xh, y2, z2}, {-xh, y2, z1}, {-xh, y1, z1}, {-xh, y1, z0},
    };
    const Vector3 right[] = {
        { xh, y0, z0}, { xh, y0, z3}, { xh, y3, z3}, { xh, y3, z2},
        { xh, y2, z2}, { xh, y2, z1}, { xh, y1, z1}, { xh, y1, z0},
    };

    u16 left_indices[8]{};
    u16 right_indices[8]{};
    for (u32 i = 0; i < 8; ++i) {
        left_indices[i] = add_contour_vertex(&geometry, left[i]);
        right_indices[i] = add_contour_vertex(&geometry, right[i]);
    }
    for (u32 i = 0; i < 8; ++i) {
        const u32 next = (i + 1) % 8;
        add_contour_edge(&geometry, left_indices[i], left_indices[next]);
        add_contour_edge(&geometry, right_indices[i], right_indices[next]);
        add_contour_edge(&geometry, left_indices[i], right_indices[i]);
    }
    return geometry;
}

static MeshGeometry make_tunnel_contour_geometry(const char* name) {
    MeshGeometry geometry{};
    std::snprintf(geometry.name, sizeof(geometry.name), "%s", name);

    constexpr float x0 = -2.0f;
    constexpr float x1 = -1.0f;
    constexpr float x2 = 1.0f;
    constexpr float x3 = 2.0f;
    constexpr float y0 = -1.0f;
    constexpr float y1 = 0.0f;
    constexpr float y2 = 1.0f;
    constexpr float z0 = -2.5f;
    constexpr float z1 = 2.5f;

    const Vector3 front[] = {
        {x0, y0, z0}, {x0, y2, z0}, {x3, y2, z0}, {x3, y0, z0},
        {x2, y0, z0}, {x2, y1, z0}, {x1, y1, z0}, {x1, y0, z0},
    };
    const Vector3 back[] = {
        {x0, y0, z1}, {x0, y2, z1}, {x3, y2, z1}, {x3, y0, z1},
        {x2, y0, z1}, {x2, y1, z1}, {x1, y1, z1}, {x1, y0, z1},
    };

    u16 front_indices[8]{};
    u16 back_indices[8]{};
    for (u32 i = 0; i < 8; ++i) {
        front_indices[i] = add_contour_vertex(&geometry, front[i]);
        back_indices[i] = add_contour_vertex(&geometry, back[i]);
    }
    for (u32 i = 0; i < 8; ++i) {
        const u32 next = (i + 1) % 8;
        add_contour_edge(&geometry, front_indices[i], front_indices[next]);
        add_contour_edge(&geometry, back_indices[i], back_indices[next]);
        add_contour_edge(&geometry, front_indices[i], back_indices[i]);
    }
    return geometry;
}

static void add_contour_mesh(Dimension* dim, u32 dimension_id, MeshGeometry geometry, const char* name, Vector3 origin, float bounds_radius) {
    const u32 geometry_id = dimension_add_geometry(dim, geometry);
    MeshGeometry* stored_geometry = arena_get(&dim->geometries, geometry_id);
    if (stored_geometry) stored_geometry->lod_geometry = geometry_id;

    MeshInstance mesh{};
    std::snprintf(mesh.name, sizeof(mesh.name), "%s", name);
    mesh.geometry = geometry_id;
    mesh.origin = demo_pos(dimension_id, origin, dim->chunk_size_m);
    mesh.se3 = MatrixIdentity();
    mesh.color = BLACK;
    mesh.bounds_radius = bounds_radius;
    mesh.lit = false;
    mesh.draw_edges = true;
    dimension_add_mesh(dim, mesh);
}

static const SavedPlayerState* find_current_session_player_state(const DemoApp* app, u64 peer_id) {
    const SavedSessionState* session = find_session(&app->profile, app->session_name);
    if (session) {
        const char* session_world = session->world_name[0] ? session->world_name : world_playground_name;
        if (std::strcmp(canonical_world_name(session_world), canonical_world_name(app->world_name)) != 0) {
            return nullptr;
        }
    }
    return find_saved_player(session, peer_id);
}

static void restore_current_session_paint(DemoApp* app, Dimension* dim) {
    if (!app || !dim) return;
    const SavedSessionState* session = find_session(&app->profile, app->session_name);
    if (!session) return;
    const char* session_world = session->world_name[0] ? session->world_name : world_playground_name;
    if (std::strcmp(canonical_world_name(session_world), canonical_world_name(app->world_name)) != 0) return;
    for (const SavedPaintPixel& saved : session->painted_pixels) {
        PaintedPixel pixel{};
        pixel.center = {app->dimension_id, saved.chunk, saved.local};
        pixel.normal = saved.normal;
        pixel.tangent = saved.tangent;
        pixel.color = saved.color;
        pixel.quad_offset = saved.quad_offset;
        pixel.quad_half_size = saved.quad_half_size;
        pixel.mesh_id = saved.mesh_id;
        pixel.sprite_id = saved.sprite_id;
        pixel.sprite_pixel_x = saved.sprite_pixel_x;
        pixel.sprite_pixel_y = saved.sprite_pixel_y;
        dimension_paint_pixel(dim, pixel);
    }
}

static WorldPos saved_player_feet_pos(u32 dimension_id, const SavedPlayerState& state) {
    WorldPos pos{};
    pos.dimension = dimension_id;
    pos.chunk = state.chunk;
    pos.local = state.local;
    return pos;
}

static void apply_saved_player_state(Dimension* dim, u32 dimension_id, PlayerEntity* player, const SavedPlayerState& state, bool restore_identity) {
    if (!dim || !player || !state.valid) return;

    if (restore_identity) {
        copy_text(player->name, sizeof(player->name), state.name[0] ? state.name : player->name);
        player->color = state.color;
    }
    player->yaw = state.yaw;
    player->pitch = state.pitch;
    player->body_radius = state.body_radius > 0.0f ? state.body_radius : player_radius_m;
    player->current_height = state.current_height > 0.0f ? state.current_height : player_stand_height_m;
    player->eye_height = player->current_height < player_stand_height_m - 0.01f ? player_crouch_eye_m : player_stand_eye_m;
    player->camera_y_offset = 0.0f;
    player->velocity = {};
    player->is_crouching = player->current_height < player_stand_height_m - 0.01f;
    player->aim_ray_active = false;
    WorldPos feet = saved_player_feet_pos(dimension_id, state);
    canonicalize(&feet, dim->chunk_size_m);
    player_sync_masses_to_pose(dim, player, feet);
}

static void respawn_player_at_default(DemoApp* app, Dimension* dim, PlayerEntity* player) {
    if (!app || !dim || !player) return;

    WorldPos spawn = default_player_spawn(app, app->dimension_id, dim->chunk_size_m);
    canonicalize(&spawn, dim->chunk_size_m);
    player->velocity = {};
    player->camera_y_offset = 0.0f;
    player->jump_hold_time = 0.0f;
    player->grounded_frames = 0;
    player->jump_variable_active = false;
    player->on_ground = false;
    player->aim_ray_active = false;
    player_sync_masses_to_pose(dim, player, spawn);
    mark_profile_dirty(app);
}

static void apply_current_session_local_spawn(DemoApp* app, Dimension* dim, PlayerEntity* player) {
    if (!app || !dim || !player) return;

    const SavedPlayerState* saved = find_current_session_player_state(app, local_player_peer_id(app));
    if (saved) {
        apply_saved_player_state(dim, app->dimension_id, player, *saved, false);
    } else {
        respawn_player_at_default(app, dim, player);
    }
}

static void add_local_player_to_generated_world(DemoApp* app, Dimension* dim) {
    WorldPos local_spawn = default_player_spawn(app, app->dimension_id, dim->chunk_size_m);
    const SavedPlayerState* saved_local = find_current_session_player_state(app, local_player_peer_id(app));
    if (saved_local) {
        local_spawn = saved_player_feet_pos(app->dimension_id, *saved_local);
        canonicalize(&local_spawn, dim->chunk_size_m);
    }

    if (app->landscape_streaming) {
        ensure_landscape_chunks_around(app, dim, local_spawn);
    }

    app->local_player_id = dimension_add_player(
        dim,
        app->player_name,
        app->player_color,
        local_spawn,
        true
    );
    if (saved_local) {
        if (PlayerEntity* player = arena_get(&dim->players, app->local_player_id)) {
            apply_saved_player_state(dim, app->dimension_id, player, *saved_local, false);
        }
    }
}

static void generate_playground_world(DemoApp* app) {
    world_init(&app->world);
    reset_remote_players(app);
    clear_streamed_chunk_records(app);
    reset_cave_animation(app);
    app->landscape_streaming = false;
    app->landscape_cube_geom = invalid_id;
    app->dimension_id = world_add_dimension(&app->world, "default", 16.0f, 64.0f);
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    dim->render_radius_chunks = 7;
    dim->quality_render_radius_chunks = 4;
    dim->ambient = 0.38f;
    dim->sky_top = {95, 142, 205, 255};
    dim->sky_bottom = {210, 229, 245, 255};
    dim->fog_color = {185, 204, 219, 255};
    apply_profile_render_radius(app, dim);

    MeshGeometry cube = make_box_geometry("unit cube", {1.0f, 1.0f, 1.0f});
    MeshGeometry cube_lod = make_box_geometry("unit cube lod", {0.96f, 0.96f, 0.96f});
    const u32 cube_lod_id = dimension_add_geometry(dim, cube_lod);
    cube.lod_geometry = cube_lod_id;
    const u32 cube_id = dimension_add_geometry(dim, cube);

    MeshGeometry wedge = make_wedge_geometry("wedge", {1.0f, 1.0f, 1.0f});
    const u32 wedge_id = dimension_add_geometry(dim, wedge);

    add_static_box(dim, app->dimension_id, cube_id, "floor", {0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f}, {118, 134, 123, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "north wall", {0.0f, 2.0f, -18.5f}, {40.0f, 4.0f, 1.0f}, {96, 105, 124, 255}, true, true);
    add_static_box(dim, app->dimension_id, cube_id, "west wall", {-18.5f, 2.0f, 0.0f}, {1.0f, 4.0f, 40.0f}, {93, 113, 123, 255}, true, true);
    add_static_box(dim, app->dimension_id, cube_id, "block a", {4.5f, 1.0f, -4.5f}, {3.0f, 2.0f, 3.0f}, {174, 96, 88, 255});
    add_static_box(dim, app->dimension_id, cube_id, "block b", {-4.0f, 1.0f, -2.5f}, {2.0f, 2.0f, 5.0f}, {96, 145, 112, 255});
    add_static_box(dim, app->dimension_id, cube_id, "tunnel left", {8.5f, 0.5f, -3.5f}, {1.0f, 1.0f, 5.0f}, {108, 120, 145, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "tunnel right", {11.5f, 0.5f, -3.5f}, {1.0f, 1.0f, 5.0f}, {108, 120, 145, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "tunnel roof", {10.0f, 1.5f, -3.5f}, {4.0f, 1.0f, 5.0f}, {93, 102, 130, 255}, true, false);
    add_contour_mesh(dim, app->dimension_id, make_tunnel_contour_geometry("tunnel contours"), "tunnel contours", {10.0f, 1.0f, -3.5f}, 3.5f);

    add_static_box(dim, app->dimension_id, cube_id, "step 1", {-8.5f, 0.0f, 5.5f}, {3.0f, 1.0f, 3.0f}, {166, 151, 92, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "step 2", {-8.5f, 0.5f, 8.5f}, {3.0f, 2.0f, 3.0f}, {174, 160, 101, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "step 3", {-8.5f, 1.0f, 11.5f}, {3.0f, 3.0f, 3.0f}, {185, 171, 112, 255}, true, false);
    add_contour_mesh(dim, app->dimension_id, make_stair_contour_geometry("stair contours"), "stair contours", {-8.5f, 0.0f, 8.5f}, 5.5f);

    MeshInstance ramp{};
    std::snprintf(ramp.name, sizeof(ramp.name), "visual ramp");
    ramp.geometry = wedge_id;
    ramp.origin = demo_pos(app->dimension_id, {7.5f, 0.5f, 6.5f}, dim->chunk_size_m);
    ramp.se3 = MatrixScale(5.0f, 1.0f, 5.0f);
    ramp.color = {117, 136, 182, 255};
    ramp.texture_id = render_texture_grid;
    ramp.bounds_radius = Vector3Length(Vector3{5.0f, 1.0f, 5.0f}) * 0.5f;
    dimension_add_mesh(dim, ramp);

    BoxCollider ramp_collider{};
    ramp_collider.pos = demo_pos(app->dimension_id, {7.5f, 0.363f, 6.524f}, dim->chunk_size_m);
    ramp_collider.half = {2.5f, 0.12f, 2.55f};
    ramp_collider.axis_aligned = false;
    ramp_collider.rotation = QuaternionFromAxisAngle({1.0f, 0.0f, 0.0f}, -std::atan2(1.05f, 5.0f));
    ramp_collider.color = ramp.color;
    physics_add_box(&dim->physics, ramp_collider);

    add_static_box(dim, app->dimension_id, cube_id, "ramp landing", {7.5f, 0.5f, 11.0f}, {5.0f, 1.0f, 4.0f}, {107, 126, 168, 255}, true, true);

    SpriteInstance sprite{};
    sprite.size = {1.2f, 1.2f};
    sprite.color = WHITE;
    sprite.texture_id = render_texture_life;

    std::snprintf(sprite.name, sizeof(sprite.name), "life vertical billboard");
    sprite.origin = demo_pos(app->dimension_id, {-3.0f, 1.6f, -7.0f}, dim->chunk_size_m);
    sprite.billboard = billboard_vertical;
    dimension_add_sprite(dim, sprite);

    std::snprintf(sprite.name, sizeof(sprite.name), "life full billboard");
    sprite.origin = demo_pos(app->dimension_id, {-1.4f, 1.6f, -7.0f}, dim->chunk_size_m);
    sprite.billboard = billboard_full_3d;
    dimension_add_sprite(dim, sprite);

    std::snprintf(sprite.name, sizeof(sprite.name), "life flat sprite");
    sprite.origin = demo_pos(app->dimension_id, {0.2f, 1.6f, -7.0f}, dim->chunk_size_m);
    sprite.se3 = MatrixRotateY(25.0f * DEG2RAD);
    sprite.billboard = billboard_none;
    dimension_add_sprite(dim, sprite);

    add_emissive_box(
        dim,
        app->dimension_id,
        cube_id,
        "playground warm emitter",
        {0.0f, 5.5f, -5.0f},
        {1.0f, 1.0f, 1.0f},
        {255, 230, 180, 255},
        7.0f);
    add_emissive_box(
        dim,
        app->dimension_id,
        cube_id,
        "playground blue emitter",
        {-8.0f, 4.0f, 8.0f},
        {1.0f, 1.0f, 1.0f},
        {120, 160, 255, 255},
        6.0f);

    restore_current_session_paint(app, dim);
    add_local_player_to_generated_world(app, dim);
}

static void generate_cave_world(DemoApp* app) {
    world_init(&app->world);
    reset_remote_players(app);
    clear_streamed_chunk_records(app);
    reset_cave_animation(app);
    app->landscape_streaming = false;
    app->landscape_cube_geom = invalid_id;
    app->dimension_id = world_add_dimension(&app->world, "cave", 16.0f, 64.0f);
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    if (!dim) return;

    dim->render_radius_chunks = 4;
    dim->quality_render_radius_chunks = 3;
    dim->ambient = 0.01f;
    dim->sky_top = {3, 5, 9, 255};
    dim->sky_bottom = {5, 7, 11, 255};
    dim->fog_color = {4, 6, 10, 255};
    apply_profile_render_radius(app, dim);

    MeshGeometry cube = make_box_geometry("cave cube", {1.0f, 1.0f, 1.0f});
    // Cave shell tiles meet exactly at chunk boundaries. Keep their fallback
    // geometry identical so crossing the quality radius cannot expose seams or
    // make an entire 16 m tile visibly shrink in one frame.
    MeshGeometry cube_lod = make_box_geometry("cave cube lod", {1.0f, 1.0f, 1.0f});
    const u32 cube_lod_id = dimension_add_geometry(dim, cube_lod);
    cube.lod_geometry = cube_lod_id;
    const u32 cube_id = dimension_add_geometry(dim, cube);

    const Color cave_floor = {62, 72, 69, 255};
    const Color cave_ceiling = {45, 52, 55, 255};
    const Color cave_wall = {54, 63, 66, 255};
    constexpr float tile_centers[] = {-8.0f, 8.0f, 24.0f};
    for (u32 x = 0; x < 3; ++x) {
        for (u32 z = 0; z < 3; ++z) {
            char name[48]{};
            std::snprintf(name, sizeof(name), "cave floor %u %u", x, z);
            add_cave_box(
                dim, app->dimension_id, cube_id, name,
                {tile_centers[x], -0.5f, tile_centers[z]}, {16.0f, 1.0f, 16.0f},
                cave_floor, false, 0.0f, false);
            std::snprintf(name, sizeof(name), "cave ceiling %u %u", x, z);
            add_cave_box(
                dim, app->dimension_id, cube_id, name,
                {tile_centers[x], 10.5f, tile_centers[z]}, {16.0f, 1.0f, 16.0f},
                cave_ceiling, false, 0.0f, false);
        }
    }

    for (u32 i = 0; i < 3; ++i) {
        char name[48]{};
        std::snprintf(name, sizeof(name), "cave west wall %u", i);
        add_cave_box(
            dim, app->dimension_id, cube_id, name,
            {-16.5f, 5.0f, tile_centers[i]}, {1.0f, 10.0f, 16.0f},
            cave_wall, false, 0.0f, false);
        std::snprintf(name, sizeof(name), "cave east wall %u", i);
        add_cave_box(
            dim, app->dimension_id, cube_id, name,
            {32.5f, 5.0f, tile_centers[i]}, {1.0f, 10.0f, 16.0f},
            cave_wall, false, 0.0f, false);
        std::snprintf(name, sizeof(name), "cave north wall %u", i);
        add_cave_box(
            dim, app->dimension_id, cube_id, name,
            {tile_centers[i], 5.0f, -16.5f}, {16.0f, 10.0f, 1.0f},
            cave_wall, false, 0.0f, false);
        std::snprintf(name, sizeof(name), "cave south wall %u", i);
        add_cave_box(
            dim, app->dimension_id, cube_id, name,
            {tile_centers[i], 5.0f, 32.5f}, {16.0f, 10.0f, 1.0f},
            cave_wall, false, 0.0f, false);
    }

    // The shell is visually tiled for chunk-local lighting, but broad collision slabs
    // avoid support discontinuities where neighboring visual tiles meet.
    add_box_collider(dim, app->dimension_id, {8.0f, -0.5f, 8.0f}, {48.0f, 1.0f, 48.0f}, cave_floor);
    add_box_collider(dim, app->dimension_id, {8.0f, 10.5f, 8.0f}, {48.0f, 1.0f, 48.0f}, cave_ceiling);
    add_box_collider(dim, app->dimension_id, {-16.5f, 5.0f, 8.0f}, {1.0f, 10.0f, 48.0f}, cave_wall);
    add_box_collider(dim, app->dimension_id, {32.5f, 5.0f, 8.0f}, {1.0f, 10.0f, 48.0f}, cave_wall);
    add_box_collider(dim, app->dimension_id, {8.0f, 5.0f, -16.5f}, {48.0f, 10.0f, 1.0f}, cave_wall);
    add_box_collider(dim, app->dimension_id, {8.0f, 5.0f, 32.5f}, {48.0f, 10.0f, 1.0f}, cave_wall);

    const Color arch = {83, 73, 79, 255};
    add_cave_box(dim, app->dimension_id, cube_id, "cave arch left", {4.0f, 3.0f, 7.0f}, {2.0f, 6.0f, 2.0f}, arch, true);
    add_cave_box(dim, app->dimension_id, cube_id, "cave arch right", {12.0f, 3.0f, 7.0f}, {2.0f, 6.0f, 2.0f}, arch, true);
    add_cave_box(dim, app->dimension_id, cube_id, "cave arch lintel", {8.0f, 7.0f, 7.0f}, {10.0f, 2.0f, 2.0f}, arch, true);

    const Color grille = {70, 82, 88, 255};
    constexpr float grille_x[] = {3.0f, 5.0f, 7.0f, 9.0f, 11.0f, 13.0f};
    for (u32 i = 0; i < static_cast<u32>(sizeof(grille_x) / sizeof(grille_x[0])); ++i) {
        char name[48]{};
        std::snprintf(name, sizeof(name), "cave shadow grille %u", i);
        add_cave_box(
            dim, app->dimension_id, cube_id, name,
            {grille_x[i], 2.5f, -1.0f}, {0.5f, 5.0f, 0.5f},
            grille, true);
    }

    const Color blocks = {76, 68, 91, 255};
    add_cave_box(dim, app->dimension_id, cube_id, "cave tall block", {20.0f, 2.0f, 18.0f}, {3.0f, 4.0f, 3.0f}, blocks, true);
    add_cave_box(dim, app->dimension_id, cube_id, "cave thin block", {24.0f, 3.0f, 15.0f}, {2.0f, 6.0f, 2.0f}, blocks, true);
    add_cave_box(dim, app->dimension_id, cube_id, "cave overhead bar", {22.0f, 7.0f, 16.0f}, {8.0f, 1.0f, 2.0f}, blocks, true);

    add_cave_box(
        dim, app->dimension_id, cube_id, "cave mirror panel",
        {31.93f, 4.0f, 8.0f}, {0.125f, 6.0f, 10.0f},
        {154, 174, 183, 255}, false, 0.95f, false);
    add_cave_box(
        dim, app->dimension_id, cube_id, "cave reflective plinth",
        {20.0f, 1.5f, 4.0f}, {3.0f, 3.0f, 3.0f},
        {111, 132, 146, 255}, true, 0.70f, true);
    add_cave_box(
        dim, app->dimension_id, cube_id, "cave reflective pool",
        {8.0f, 0.02f, -8.0f}, {12.0f, 0.04f, 6.0f},
        {76, 104, 116, 255}, false, 0.85f, false);

    app->cave_emissive_base_positions = {
        Vector3{2.0f, 6.0f, 20.0f},
        Vector3{14.0f, 5.0f, 8.0f},
        Vector3{24.0f, 7.0f, -5.0f},
    };
    app->cave_emissive_mesh_ids[0] = add_emissive_box(
        dim, app->dimension_id, cube_id, "cave warm flying emitter",
        app->cave_emissive_base_positions[0], {1.5f, 1.5f, 1.5f},
        {255, 150, 55, 255}, 8.0f);
    app->cave_emissive_mesh_ids[1] = add_emissive_box(
        dim, app->dimension_id, cube_id, "cave cyan flying emitter",
        app->cave_emissive_base_positions[1], {1.0f, 1.0f, 1.0f},
        {70, 205, 255, 255}, 10.0f);
    app->cave_emissive_mesh_ids[2] = add_emissive_box(
        dim, app->dimension_id, cube_id, "cave violet flying emitter",
        app->cave_emissive_base_positions[2], {2.0f, 2.0f, 2.0f},
        {190, 90, 255, 255}, 7.0f);

    restore_current_session_paint(app, dim);
    add_local_player_to_generated_world(app, dim);
}

static void generate_hills_world(DemoApp* app) {
    world_init(&app->world);
    reset_remote_players(app);
    clear_streamed_chunk_records(app);
    reset_cave_animation(app);
    app->landscape_streaming = true;
    app->dimension_id = world_add_dimension(&app->world, "hills", 16.0f, 64.0f);
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    if (!dim) return;

    dim->render_radius_chunks = 5;
    dim->quality_render_radius_chunks = 3;
    dim->ambient = 0.54f;
    dim->sky_top = {112, 158, 214, 255};
    dim->sky_bottom = {214, 230, 238, 255};
    dim->fog_color = {180, 202, 212, 255};
    apply_profile_render_radius(app, dim);

    MeshGeometry cube = make_box_geometry("stream cube", {1.0f, 1.0f, 1.0f});
    MeshGeometry cube_lod = make_box_geometry("stream cube lod", {0.96f, 0.96f, 0.96f});
    const u32 cube_lod_id = dimension_add_geometry(dim, cube_lod);
    cube.lod_geometry = cube_lod_id;
    app->landscape_cube_geom = dimension_add_geometry(dim, cube);

    restore_current_session_paint(app, dim);
    add_local_player_to_generated_world(app, dim);
}

void demo_generate_world(DemoApp* app) {
    char normalized[32]{};
    copy_world_name(normalized, sizeof(normalized), app->world_name);
    copy_text(app->world_name, sizeof(app->world_name), normalized);
    if (is_landscape_world(app->world_name)) {
        generate_hills_world(app);
    } else if (is_cave_world(app->world_name)) {
        generate_cave_world(app);
    } else {
        generate_playground_world(app);
    }
}

void demo_init(DemoApp* app) {
#if defined(OL_DISABLE_RAYLIB_LOGGING)
    SetTraceLogLevel(LOG_NONE);
#endif
    app->dimension_id = invalid_id;
    app->local_player_id = invalid_id;
    app->fixed_accum = 0.0f;
    app->in_game = false;
    app->mouse_captured = false;
    app->paused = false;
    app->blocking_screen_cursor_released = false;
    app->active_menu_field = menu_input_player;
    app->dragged_menu_control = menu_control_none;
    app->dragged_pause_control = pause_control_none;
    app->color_picker_open = false;
    app->sessions_open = false;
    app->worlds_open = false;
    app->session_scroll = 0;
    app->deleting_session_index = -1;
    app->delete_hold_active = false;
    app->restore_sent_peer_ids.fill(0);
    app->frame_input = {};
    app->previous_key_down.fill(false);
    app->previous_mouse_left_down = false;
    app->landscape_stream_center_valid = false;
    std::snprintf(app->player_name, sizeof(app->player_name), "player");
    std::snprintf(app->session_name, sizeof(app->session_name), "session");
    std::snprintf(app->world_name, sizeof(app->world_name), "%s", world_playground_name);
    app->player_color = {90, 180, 255, 255};
    load_profile(app);
    start_profile_save_worker(app);
    InitWindow(1280, 720, "OL");
    SetExitKey(KEY_NULL);
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    set_fullscreen_enabled(app, app->profile.fullscreen, false);
    SetTargetFPS(120);
    renderer_init(&app->renderer);
    apply_profile_lighting_settings(app);
    net_init(&app->net);
    demo_generate_world(app);
}

void demo_shutdown(DemoApp* app) {
    record_current_world_state(app);
    save_profile(app);
    stop_profile_save_worker(app);
    net_shutdown(&app->net);
    renderer_shutdown(&app->renderer);
    CloseWindow();
}

void demo_draw_menu(DemoApp* app) {
    const char* session_names[max_saved_sessions]{};
    for (u32 i = 0; i < app->profile.session_count; ++i) {
        session_names[i] = app->profile.sessions[i].name;
    }
    const char* world_names[world_choice_count]{};
    for (u32 i = 0; i < world_choice_count; ++i) {
        world_names[i] = world_choices[i];
    }
    const float delete_progress = app->delete_hold_active
        ? clampf(static_cast<float>((GetTime() - app->delete_hold_started) / 3.0), 0.0f, 1.0f)
        : 0.0f;

    demo_draw_menu_screen(MenuScreen{
        &app->renderer,
        app->player_name,
        app->session_name,
        app->world_name,
        app->net.status,
        app->player_color,
        app->active_menu_field,
        app->color_picker_open,
        app->sessions_open,
        app->worlds_open,
        session_names,
        app->profile.session_count,
        world_names,
        world_choice_count,
        app->session_scroll,
        app->deleting_session_index,
        delete_progress
    });
}

static void enter_game(DemoApp* app) {
    demo_generate_world(app);
    app->in_game = true;
    app->paused = false;
    app->mouse_captured = true;
    app->blocking_screen_cursor_released = false;
    app->joining_screen_active = false;
    app->joining_restore_applied = false;
    app->joining_paint_history_complete = false;
    app->dragged_pause_control = pause_control_none;
    app->sessions_open = false;
    app->worlds_open = false;
    app->restore_sent_peer_ids.fill(0);
    DisableCursor();
}

static void host_game(DemoApp* app) {
    upsert_session(&app->profile, app->session_name, app->world_name);
    mark_profile_dirty(app);
    save_profile(app);
    net_host(&app->net, app->session_name, app->world_name);
    enter_game(app);
}

static void join_game(DemoApp* app) {
    upsert_session(&app->profile, app->session_name, app->world_name);
    mark_profile_dirty(app);
    save_profile(app);
    net_join_from_clipboard(&app->net);
    if (app->net.pending || app->net.in_lobby) {
        enter_game(app);
        app->joining_screen_active = true;
        app->joining_restore_applied = false;
        app->joining_paint_history_complete = false;
        app->joining_started = GetTime();
    }
}

static void cancel_session_delete(DemoApp* app) {
    app->delete_hold_active = false;
    app->delete_hold_started = 0.0;
    app->deleting_session_index = -1;
}

static void update_session_delete_hold(DemoApp* app) {
    if (!app->delete_hold_active) return;
    if (!app->frame_input.mouse_left_down) {
        cancel_session_delete(app);
        return;
    }

    const MenuHit hit = demo_menu_hit_test(
        app->color_picker_open,
        app->sessions_open,
        app->worlds_open,
        app->session_scroll,
        app->profile.session_count,
        world_choice_count,
        GetMousePosition());
    if (hit.control != menu_control_session_delete || hit.session_index != app->deleting_session_index) {
        cancel_session_delete(app);
        return;
    }

    if (GetTime() - app->delete_hold_started >= 3.0) {
        remove_session_at(app, app->deleting_session_index);
        save_profile(app);
    }
}

static void update_menu(DemoApp* app) {
    if (app->sessions_open && app->profile.session_count > max_visible_session_rows) {
        const float wheel = GetMouseWheelMove();
        if (wheel > 0.0f) app->session_scroll--;
        if (wheel < 0.0f) app->session_scroll++;
        clamp_session_scroll(app);
    }

    if (app->frame_input.mouse_left_pressed) {
        const MenuHit hit = demo_menu_hit_test(
            app->color_picker_open,
            app->sessions_open,
            app->worlds_open,
            app->session_scroll,
            app->profile.session_count,
            world_choice_count,
            GetMousePosition());
        app->dragged_menu_control = menu_control_none;

        if (hit.control == menu_control_player) {
            app->active_menu_field = menu_input_player;
            app->sessions_open = false;
            app->worlds_open = false;
        } else if (hit.control == menu_control_session) {
            app->active_menu_field = menu_input_session;
            app->worlds_open = false;
        } else if (hit.control == menu_control_session_dropdown) {
            app->sessions_open = !app->sessions_open;
            app->worlds_open = false;
            app->color_picker_open = false;
            clamp_session_scroll(app);
        } else if (hit.control == menu_control_session_item && hit.session_index >= 0) {
            const SavedSessionState& session = app->profile.sessions[static_cast<u32>(hit.session_index)];
            copy_text(app->session_name, sizeof(app->session_name), session.name);
            copy_text(app->profile.session_name, sizeof(app->profile.session_name), app->session_name);
            copy_world_name(app->world_name, sizeof(app->world_name), session.world_name);
            copy_world_name(app->profile.world_name, sizeof(app->profile.world_name), app->world_name);
            app->active_menu_field = menu_input_session;
            app->sessions_open = false;
            app->worlds_open = false;
            mark_profile_dirty(app);
        } else if (hit.control == menu_control_session_delete && hit.session_index >= 0) {
            app->delete_hold_active = true;
            app->delete_hold_started = GetTime();
            app->deleting_session_index = hit.session_index;
        } else if (hit.control == menu_control_world_dropdown) {
            app->worlds_open = !app->worlds_open;
            app->sessions_open = false;
            app->color_picker_open = false;
        } else if (hit.control == menu_control_world_item && hit.world_index >= 0 && static_cast<u32>(hit.world_index) < world_choice_count) {
            copy_world_name(app->world_name, sizeof(app->world_name), world_choices[static_cast<u32>(hit.world_index)]);
            copy_world_name(app->profile.world_name, sizeof(app->profile.world_name), app->world_name);
            if (SavedSessionState* session = find_session(&app->profile, app->session_name)) {
                copy_world_name(session->world_name, sizeof(session->world_name), app->world_name);
            }
            app->worlds_open = false;
            mark_profile_dirty(app);
        } else if (hit.control == menu_control_color) {
            app->color_picker_open = !app->color_picker_open;
            app->sessions_open = false;
            app->worlds_open = false;
        } else if (hit.control == menu_control_host) {
            host_game(app);
            return;
        } else if (hit.control == menu_control_join) {
            join_game(app);
            return;
        } else if (hit.control == menu_control_color_r || hit.control == menu_control_color_g || hit.control == menu_control_color_b) {
            app->dragged_menu_control = hit.control;
            set_player_color_component(app, hit.control, demo_menu_color_value_from_mouse(hit.control, GetMousePosition()));
            mark_profile_dirty(app);
        } else {
            app->active_menu_field = menu_input_none;
            if (app->color_picker_open) app->color_picker_open = false;
            if (app->sessions_open) app->sessions_open = false;
            if (app->worlds_open) app->worlds_open = false;
        }
    }

    if (app->frame_input.mouse_left_down &&
        (app->dragged_menu_control == menu_control_color_r ||
         app->dragged_menu_control == menu_control_color_g ||
         app->dragged_menu_control == menu_control_color_b)) {
        set_player_color_component(app, app->dragged_menu_control, demo_menu_color_value_from_mouse(app->dragged_menu_control, GetMousePosition()));
        mark_profile_dirty(app);
    }
    if (app->frame_input.mouse_left_released) {
        app->dragged_menu_control = menu_control_none;
        if (app->delete_hold_active) cancel_session_delete(app);
    }

    update_session_delete_hold(app);

    if (app->frame_input.tab_pressed) {
        app->active_menu_field = app->active_menu_field == menu_input_player ? menu_input_session : menu_input_player;
        app->sessions_open = false;
        app->worlds_open = false;
    }

    size_t cap = 0;
    char* text = active_menu_text(app, &cap);
    int c = GetCharPressed();
    bool text_changed = false;
    while (c) {
        if (text) {
            add_text_char(text, cap, c);
            text_changed = true;
        }
        c = GetCharPressed();
    }
    if (text && app->frame_input.backspace_pressed) {
        remove_text_char(text);
        text_changed = true;
    }
    if (text_changed) {
        copy_text(app->profile.player_name, sizeof(app->profile.player_name), app->player_name);
        copy_text(app->profile.session_name, sizeof(app->profile.session_name), app->session_name);
        app->profile.player_color = app->player_color;
        app->sessions_open = false;
        app->worlds_open = false;
        mark_profile_dirty(app);
    }
}

static PlayerEntity* local_player(DemoApp* app, Dimension* dim) {
    return arena_get(&dim->players, app->local_player_id);
}

static void collect_input(DemoApp* app, PlayerEntity* player) {
    if (!app->mouse_captured && (app->frame_input.mouse_left_pressed || app->frame_input.mouse_right_down)) {
        app->mouse_captured = true;
        DisableCursor();
    }

    if (app->mouse_captured) {
        const Vector2 mouse = GetMouseDelta();
        const float sensitivity = 0.0022f;
        player->yaw += mouse.x * sensitivity;
        player->pitch -= mouse.y * sensitivity;
        player->pitch = clampf(player->pitch, -1.45f, 1.45f);
    }

    Vector2 move{};
    if (IsKeyDown(KEY_W)) move.y += 1.0f;
    if (IsKeyDown(KEY_S)) move.y -= 1.0f;
    if (IsKeyDown(KEY_D)) move.x += 1.0f;
    if (IsKeyDown(KEY_A)) move.x -= 1.0f;
    if (Vector2Length(move) > 1.0f) move = Vector2Normalize(move);
    app->input.move = move;
    app->input.jump_pressed = app->input.jump_pressed || app->frame_input.space_pressed;
    app->input.jump_held = IsKeyDown(KEY_SPACE);
    app->input.sprint = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    app->input.crouch = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
}

static bool ray_intersects_triangle(Ray ray, Vector3 a, Vector3 b, Vector3 c, float max_dist, float* out_t) {
    constexpr float eps = 0.00001f;
    const Vector3 e1 = b - a;
    const Vector3 e2 = c - a;
    const Vector3 h = Vector3CrossProduct(ray.direction, e2);
    const float det = Vector3DotProduct(e1, h);
    if (std::fabs(det) < eps) return false;

    const float inv_det = 1.0f / det;
    const Vector3 s = ray.position - a;
    const float u = inv_det * Vector3DotProduct(s, h);
    if (u < -eps || u > 1.0f + eps) return false;

    const Vector3 q = Vector3CrossProduct(s, e1);
    const float v = inv_det * Vector3DotProduct(ray.direction, q);
    if (v < -eps || u + v > 1.0f + eps) return false;

    const float t = inv_det * Vector3DotProduct(e2, q);
    if (t <= 0.01f || t >= max_dist) return false;
    if (out_t) *out_t = t;
    return true;
}

static bool ray_hits_player_cylinder(Ray ray, Vector3 bottom, float height, float radius, float max_dist, float* out_t) {
    const float min_y = bottom.y;
    const float max_y = bottom.y + height;
    const float ox = ray.position.x - bottom.x;
    const float oz = ray.position.z - bottom.z;
    const float dx = ray.direction.x;
    const float dz = ray.direction.z;
    const float radius_sq = radius * radius;

    float best_t = std::numeric_limits<float>::max();
    const float a = dx * dx + dz * dz;
    const float b = 2.0f * (ox * dx + oz * dz);
    const float c = ox * ox + oz * oz - radius_sq;
    if (a > 0.000001f) {
        const float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            const float root = std::sqrt(disc);
            const float inv = 0.5f / a;
            const float candidates[2] = {(-b - root) * inv, (-b + root) * inv};
            for (float t : candidates) {
                const float y = ray.position.y + ray.direction.y * t;
                if (t > 0.01f && t < max_dist && y >= min_y && y <= max_y && t < best_t) best_t = t;
            }
        }
    }

    if (std::fabs(ray.direction.y) > 0.000001f) {
        const float cap_y[2] = {min_y, max_y};
        for (float y_plane : cap_y) {
            const float t = (y_plane - ray.position.y) / ray.direction.y;
            const float x = ray.position.x + ray.direction.x * t - bottom.x;
            const float z = ray.position.z + ray.direction.z * t - bottom.z;
            if (t > 0.01f && t < max_dist && x * x + z * z <= radius_sq && t < best_t) best_t = t;
        }
    }

    if (best_t == std::numeric_limits<float>::max()) return false;
    if (out_t) *out_t = best_t;
    return true;
}

struct WorldRayHit {
    float distance = 0.0f;
    u32 mesh_id = invalid_id;
    Vector3 point{};
    Vector3 normal = {0.0f, 1.0f, 0.0f};
    Vector3 mesh_origin{};
    u32 sprite_id = invalid_id;
    i32 sprite_pixel_x = 0;
    i32 sprite_pixel_y = 0;
};

static constexpr float paint_max_ray_dist_m = 10.0f;

static const Texture2D* demo_texture_handle(const DemoApp* app, u32 texture_id) {
    if (texture_id == render_texture_life) return &app->renderer.life_texture;
    if (texture_id == render_texture_cross) return &app->renderer.cross_texture;
    if (texture_id == render_texture_grid) return &app->renderer.grid_texture;
    if (texture_id == render_texture_grass) return &app->renderer.grass_texture;
    if (texture_id == render_texture_stone) return &app->renderer.stone_texture;
    if (texture_id == render_texture_roof) return &app->renderer.roof_texture;
    return nullptr;
}

static Vector2 demo_texture_pixel_size(const DemoApp* app, u32 texture_id) {
    const Texture2D* texture = demo_texture_handle(app, texture_id);
    if (texture && IsTextureValid(*texture)) return {static_cast<float>(texture->width), static_cast<float>(texture->height)};
    return {16.0f, 16.0f};
}

static bool demo_texture_pixel_is_opaque(const DemoApp* app, u32 texture_id, i32 x, i32 y) {
    const Texture2D* texture = demo_texture_handle(app, texture_id);
    if (!texture || !IsTextureValid(*texture)) return true;
    if (x < 0 || y < 0 || x >= texture->width || y >= texture->height) return false;
    const std::vector<u8>* alpha = nullptr;
    if (texture_id == render_texture_life) alpha = &app->renderer.life_alpha;
    else if (texture_id == render_texture_cross) alpha = &app->renderer.cross_alpha;
    if (!alpha || alpha->size() != static_cast<size_t>(texture->width) * static_cast<size_t>(texture->height)) return true;
    return (*alpha)[static_cast<size_t>(y) * static_cast<size_t>(texture->width) + static_cast<size_t>(x)] >= 128;
}

struct SpriteRayQuad {
    Vector3 center{};
    Vector3 right{};
    Vector3 up{};
    float width = 1.0f;
    float height = 1.0f;
};

static SpriteRayQuad make_sprite_ray_quad(const DemoApp* app, const Dimension* dim, const SpriteInstance* sprite, Vector3 center, Vector3 camera_forward) {
    SpriteRayQuad quad{};
    quad.center = center;
    quad.right = {1.0f, 0.0f, 0.0f};
    quad.up = {0.0f, 1.0f, 0.0f};
    if (sprite->billboard == billboard_full_3d) {
        const Vector3 forward = safe_norm(center * -1.0f, camera_forward * -1.0f);
        quad.right = safe_norm(Vector3CrossProduct({0.0f, 1.0f, 0.0f}, forward), {1.0f, 0.0f, 0.0f});
        quad.up = safe_norm(Vector3CrossProduct(forward, quad.right), {0.0f, 1.0f, 0.0f});
    } else if (sprite->billboard == billboard_vertical) {
        Vector3 to_camera = center * -1.0f;
        to_camera.y = 0.0f;
        const Vector3 forward = safe_norm(to_camera, {0.0f, 0.0f, 1.0f});
        quad.right = safe_norm(Vector3CrossProduct({0.0f, 1.0f, 0.0f}, forward), {1.0f, 0.0f, 0.0f});
    } else {
        const Matrix basis = matrix_no_translation(sprite->se3);
        quad.right = safe_norm(Vector3Transform({1.0f, 0.0f, 0.0f}, basis), {1.0f, 0.0f, 0.0f});
        quad.up = safe_norm(Vector3Transform({0.0f, 1.0f, 0.0f}, basis), {0.0f, 1.0f, 0.0f});
    }
    const Vector2 pixels = demo_texture_pixel_size(app, sprite->texture_id);
    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    quad.width = pixels.x / ppm * sprite->size.x;
    quad.height = pixels.y / ppm * sprite->size.y;
    return quad;
}

static WorldRayHit raycast_world(DemoApp* app, Dimension* dim, const PlayerEntity* local, WorldPos ray_start, Vector3 ray_dir, float max_dist) {
    WorldRayHit hit{};
    hit.distance = max_dist;
    if (!dim) return hit;

    Ray ray{};
    ray.position = {};
    ray.direction = ray_dir;
    float best_t = max_dist;

    for (u32 slot = 0; slot < dim->meshes.count; ++slot) {
        const MeshInstance* mesh = &dim->meshes.data[slot];
        if (!mesh->visible) continue;
        const MeshGeometry* geometry = arena_get(&dim->geometries, mesh->geometry);
        if (!geometry) continue;

        const Vector3 origin = world_delta_meters(mesh->origin, ray_start, dim->chunk_size_m);
        if (safe_len(origin) - mesh->bounds_radius > best_t) continue;

        Vector3 transformed[max_vertices_per_geometry]{};
        const Matrix basis = matrix_no_translation(mesh->se3);
        for (u32 i = 0; i < geometry->vertex_count; ++i) {
            transformed[i] = Vector3Transform(geometry->vertices[i], basis) + origin;
        }
        for (u32 i = 0; i < geometry->triangle_count; ++i) {
            const Triangle tri = geometry->triangles[i];
            float t = 0.0f;
            if (ray_intersects_triangle(ray, transformed[tri.a], transformed[tri.b], transformed[tri.c], best_t, &t)) {
                best_t = t;
                hit.distance = t;
                hit.mesh_id = arena_id_at_slot(&dim->meshes, slot);
                hit.point = ray.direction * t;
                hit.normal = safe_norm(Vector3CrossProduct(transformed[tri.b] - transformed[tri.a], transformed[tri.c] - transformed[tri.a]));
                if (Vector3DotProduct(hit.normal, ray.direction) > 0.0f) hit.normal = hit.normal * -1.0f;
                hit.mesh_origin = origin;
            }
        }
    }

    for (u32 slot = 0; slot < dim->sprites.count; ++slot) {
        const SpriteInstance* sprite = &dim->sprites.data[slot];
        if (!sprite->visible) continue;
        const Vector3 center = world_delta_meters(sprite->origin, ray_start, dim->chunk_size_m);
        const SpriteRayQuad quad = make_sprite_ray_quad(app, dim, sprite, center, ray.direction);
        const Vector3 half_right = quad.right * (quad.width * 0.5f);
        const Vector3 half_up = quad.up * (quad.height * 0.5f);
        const Vector3 a = center - half_right - half_up;
        const Vector3 b = center + half_right - half_up;
        const Vector3 c = center + half_right + half_up;
        const Vector3 d = center - half_right + half_up;
        float t = 0.0f;
        if (!ray_intersects_triangle(ray, a, b, c, best_t, &t) && !ray_intersects_triangle(ray, a, c, d, best_t, &t)) continue;
        const Vector3 sprite_hit_point = ray.direction * t;
        const Vector3 local = sprite_hit_point - center;
        const Vector2 pixels = demo_texture_pixel_size(app, sprite->texture_id);
        const float u = clampf(Vector3DotProduct(local, quad.right) / quad.width + 0.5f, 0.0f, 0.999999f);
        const float v = clampf(0.5f - Vector3DotProduct(local, quad.up) / quad.height, 0.0f, 0.999999f);
        const i32 pixel_x = static_cast<i32>(std::floor(u * pixels.x));
        const i32 pixel_y = static_cast<i32>(std::floor(v * pixels.y));
        if (!demo_texture_pixel_is_opaque(app, sprite->texture_id, pixel_x, pixel_y)) continue;
        best_t = t;
        hit.distance = t;
        hit.mesh_id = invalid_id;
        hit.sprite_id = arena_id_at_slot(&dim->sprites, slot);
        hit.sprite_pixel_x = pixel_x;
        hit.sprite_pixel_y = pixel_y;
        hit.point = sprite_hit_point;
    }

    for (u32 slot = 0; slot < dim->players.count; ++slot) {
        const u32 player_id = arena_id_at_slot(&dim->players, slot);
        const PlayerEntity* player = &dim->players.data[slot];
        if (!player->connected || player == local || player_id == app->local_player_id) continue;

        const WorldPos feet = player_feet_pos(dim, player);
        const Vector3 bottom = world_delta_meters(feet, ray_start, dim->chunk_size_m);
        const float radius = fmaxf(0.01f, player->body_radius);
        const float height = fmaxf(radius * 2.0f, player->current_height);
        float t = 0.0f;
        if (ray_hits_player_cylinder(ray, bottom, height, radius, best_t, &t)) {
            best_t = t;
            hit.distance = t;
            hit.mesh_id = invalid_id;
        }
    }

    return hit;
}

static Vector3 paint_face_tangent(Vector3 normal) {
    const Vector3 abs_n = {std::fabs(normal.x), std::fabs(normal.y), std::fabs(normal.z)};
    if (abs_n.y >= abs_n.x && abs_n.y >= abs_n.z) return {1.0f, 0.0f, 0.0f};
    if (abs_n.x >= abs_n.z) return {0.0f, 0.0f, normal.x >= 0.0f ? 1.0f : -1.0f};
    return {normal.z >= 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f};
}

static bool paint_targeted_pixel(DemoApp* app, Dimension* dim, WorldPos ray_start, const WorldRayHit& hit, Color color) {
    if (!app || !dim || (!id_valid(hit.mesh_id) && !id_valid(hit.sprite_id))) return false;
    if (id_valid(hit.sprite_id)) {
        const SpriteInstance* sprite = arena_get(&dim->sprites, hit.sprite_id);
        if (!sprite) return false;
        if (app->last_drag_pixel_valid && app->last_drag_sprite_id == hit.sprite_id &&
            app->last_drag_sprite_x == hit.sprite_pixel_x && app->last_drag_sprite_y == hit.sprite_pixel_y) return false;

        const int start_x = app->last_drag_pixel_valid && app->last_drag_sprite_id == hit.sprite_id
            ? app->last_drag_sprite_x : hit.sprite_pixel_x;
        const int start_y = app->last_drag_pixel_valid && app->last_drag_sprite_id == hit.sprite_id
            ? app->last_drag_sprite_y : hit.sprite_pixel_y;
        const int dx = hit.sprite_pixel_x - start_x;
        const int dy = hit.sprite_pixel_y - start_y;
        const int steps = std::max(std::abs(dx), std::abs(dy));
        bool painted = false;
        for (int i = 1; i <= steps; ++i) {
            PaintedPixel pixel{};
            pixel.center = sprite->origin;
            pixel.sprite_id = hit.sprite_id;
            pixel.sprite_pixel_x = start_x + static_cast<int>(std::round(static_cast<float>(dx * i) / static_cast<float>(steps)));
            pixel.sprite_pixel_y = start_y + static_cast<int>(std::round(static_cast<float>(dy * i) / static_cast<float>(steps)));
            pixel.color = color;
            if (!dimension_paint_pixel(dim, pixel)) continue;
            NetPaintPixel net_pixel{};
            net_pixel.chunk = pixel.center.chunk;
            net_pixel.local = pixel.center.local;
            net_pixel.color = color;
            net_pixel.sprite_id = pixel.sprite_id;
            net_pixel.sprite_pixel_x = pixel.sprite_pixel_x;
            net_pixel.sprite_pixel_y = pixel.sprite_pixel_y;
            net_send_paint_pixel(&app->net, net_pixel);
            painted = true;
        }
        if (steps == 0) {
            PaintedPixel pixel{};
            pixel.center = sprite->origin;
            pixel.sprite_id = hit.sprite_id;
            pixel.sprite_pixel_x = hit.sprite_pixel_x;
            pixel.sprite_pixel_y = hit.sprite_pixel_y;
            pixel.color = color;
            painted = dimension_paint_pixel(dim, pixel);
            if (painted) {
                NetPaintPixel net_pixel{};
                net_pixel.chunk = pixel.center.chunk;
                net_pixel.local = pixel.center.local;
                net_pixel.color = color;
                net_pixel.sprite_id = pixel.sprite_id;
                net_pixel.sprite_pixel_x = pixel.sprite_pixel_x;
                net_pixel.sprite_pixel_y = pixel.sprite_pixel_y;
                net_send_paint_pixel(&app->net, net_pixel);
            }
        }
        if (!painted) return false;
        app->last_drag_pixel_valid = true;
        app->last_drag_sprite_id = hit.sprite_id;
        app->last_drag_sprite_x = hit.sprite_pixel_x;
        app->last_drag_sprite_y = hit.sprite_pixel_y;
        mark_profile_dirty(app);
        return true;
    }
    const float ppm = fmaxf(dim->pixels_per_meter, 1.0f);
    const Vector3 tangent = paint_face_tangent(hit.normal);
    const Vector3 bitangent = safe_norm(Vector3CrossProduct(hit.normal, tangent), {0.0f, 0.0f, 1.0f});
    const Vector3 from_origin = hit.point - hit.mesh_origin;
    const float u = Vector3DotProduct(from_origin, tangent);
    const float v = Vector3DotProduct(from_origin, bitangent);
    const float pixel_u = (std::floor(u * ppm) + 0.5f) / ppm;
    const float pixel_v = (std::floor(v * ppm) + 0.5f) / ppm;
    Vector3 center = hit.point + tangent * (pixel_u - u) + bitangent * (pixel_v - v);

    float face_min_u = std::numeric_limits<float>::max();
    float face_max_u = -std::numeric_limits<float>::max();
    float face_min_v = std::numeric_limits<float>::max();
    float face_max_v = -std::numeric_limits<float>::max();
    if (const MeshInstance* mesh = arena_get(&dim->meshes, hit.mesh_id)) {
        if (const MeshGeometry* geometry = arena_get(&dim->geometries, mesh->geometry)) {
            const Matrix basis = matrix_no_translation(mesh->se3);
            for (u32 i = 0; i < geometry->vertex_count; ++i) {
                const Vector3 vertex = Vector3Transform(geometry->vertices[i], basis) + hit.mesh_origin;
                if (std::fabs(Vector3DotProduct(vertex - hit.point, hit.normal)) > 0.01f) continue;
                const Vector3 vertex_from_origin = vertex - hit.mesh_origin;
                const float vertex_u = Vector3DotProduct(vertex_from_origin, tangent);
                const float vertex_v = Vector3DotProduct(vertex_from_origin, bitangent);
                face_min_u = fminf(face_min_u, vertex_u);
                face_max_u = fmaxf(face_max_u, vertex_u);
                face_min_v = fminf(face_min_v, vertex_v);
                face_max_v = fmaxf(face_max_v, vertex_v);
            }
        }
    }

    const WorldPos pixel_center = worldpos_offset(ray_start, center, dim->chunk_size_m);
    if (app->last_drag_pixel_valid && Vector3DotProduct(app->last_drag_pixel_normal, hit.normal) > 0.999f &&
        safe_len(world_delta_meters(app->last_drag_pixel_center, pixel_center, dim->chunk_size_m)) < 0.25f / ppm) {
        return false;
    }

    auto commit_pixel = [&](WorldPos center_pos) {
        PaintedPixel pixel{};
        pixel.mesh_id = hit.mesh_id;
        pixel.center = center_pos;
        pixel.normal = hit.normal;
        pixel.tangent = tangent;
        pixel.color = color;
        const Vector3 center_rel = world_delta_meters(center_pos, ray_start, dim->chunk_size_m);
        const Vector3 center_from_origin = center_rel - hit.mesh_origin;
        const float center_u = Vector3DotProduct(center_from_origin, tangent);
        const float center_v = Vector3DotProduct(center_from_origin, bitangent);
        const float texel_half = 0.5f / ppm;
        const float clipped_min_u = fmaxf(center_u - texel_half, face_min_u);
        const float clipped_max_u = fminf(center_u + texel_half, face_max_u);
        const float clipped_min_v = fmaxf(center_v - texel_half, face_min_v);
        const float clipped_max_v = fminf(center_v + texel_half, face_max_v);
        if (clipped_max_u <= clipped_min_u || clipped_max_v <= clipped_min_v) return false;
        pixel.quad_offset = {
            (clipped_min_u + clipped_max_u) * 0.5f - center_u,
            (clipped_min_v + clipped_max_v) * 0.5f - center_v};
        pixel.quad_half_size = {
            (clipped_max_u - clipped_min_u) * 0.5f,
            (clipped_max_v - clipped_min_v) * 0.5f};
        if (!dimension_paint_pixel(dim, pixel)) return false;
        NetPaintPixel net_pixel{};
        net_pixel.chunk = pixel.center.chunk;
        net_pixel.local = pixel.center.local;
        net_pixel.normal = pixel.normal;
        net_pixel.tangent = pixel.tangent;
        net_pixel.color = pixel.color;
        net_pixel.quad_offset = pixel.quad_offset;
        net_pixel.quad_half_size = pixel.quad_half_size;
        net_pixel.mesh_id = pixel.mesh_id;
        net_send_paint_pixel(&app->net, net_pixel);
        return true;
    };

    bool painted = false;
    if (app->last_drag_pixel_valid && Vector3DotProduct(app->last_drag_pixel_normal, hit.normal) > 0.999f) {
        const Vector3 delta = world_delta_meters(pixel_center, app->last_drag_pixel_center, dim->chunk_size_m);
        const int du = static_cast<int>(std::round(Vector3DotProduct(delta, tangent) * ppm));
        const int dv = static_cast<int>(std::round(Vector3DotProduct(delta, bitangent) * ppm));
        const int steps = std::max(std::abs(du), std::abs(dv));
        if (std::fabs(Vector3DotProduct(delta, hit.normal)) < 0.02f && steps > 0 && steps <= 256) {
            for (int i = 1; i <= steps; ++i) {
                const int iu = static_cast<int>(std::round(static_cast<float>(du * i) / static_cast<float>(steps)));
                const int iv = static_cast<int>(std::round(static_cast<float>(dv * i) / static_cast<float>(steps)));
                const WorldPos stroke_pixel = worldpos_offset(
                    app->last_drag_pixel_center,
                    tangent * (static_cast<float>(iu) / ppm) + bitangent * (static_cast<float>(iv) / ppm),
                    dim->chunk_size_m);
                painted = commit_pixel(stroke_pixel) || painted;
            }
        }
    }
    if (!painted) painted = commit_pixel(pixel_center);
    if (!painted) return false;
    app->last_drag_pixel_valid = true;
    app->last_drag_sprite_id = invalid_id;
    app->last_drag_pixel_center = pixel_center;
    app->last_drag_pixel_normal = hit.normal;
    mark_profile_dirty(app);
    return true;
}

static void apply_received_paint_pixels(DemoApp* app, Dimension* dim) {
    NetPaintPixel received{};
    while (app && dim && net_take_paint_pixel(&app->net, &received)) {
        if (received.erase_radius_pixels < 0.0f) {
            app->joining_paint_history_complete = true;
            continue;
        }
        PaintedPixel pixel{};
        pixel.center = {app->dimension_id, received.chunk, received.local};
        pixel.normal = received.normal;
        pixel.tangent = received.tangent;
        pixel.color = received.color;
        pixel.quad_offset = received.quad_offset;
        pixel.quad_half_size = received.quad_half_size;
        pixel.mesh_id = received.mesh_id;
        pixel.sprite_id = received.sprite_id;
        pixel.sprite_pixel_x = received.sprite_pixel_x;
        pixel.sprite_pixel_y = received.sprite_pixel_y;
        if (received.erase_radius_pixels > 0.0f) dimension_erase_pixels(dim, pixel, received.erase_radius_pixels);
        else dimension_paint_pixel(dim, pixel);
    }
}

static bool erase_targeted_pixels(DemoApp* app, Dimension* dim, WorldPos ray_start, const WorldRayHit& hit) {
    if (!app || !dim || (!id_valid(hit.mesh_id) && !id_valid(hit.sprite_id))) return false;
    PaintedPixel target{};
    target.center = worldpos_offset(ray_start, hit.point, dim->chunk_size_m);
    target.normal = hit.normal;
    target.mesh_id = hit.mesh_id;
    target.sprite_id = hit.sprite_id;
    target.sprite_pixel_x = hit.sprite_pixel_x;
    target.sprite_pixel_y = hit.sprite_pixel_y;
    if (id_valid(hit.sprite_id)) {
        if (const SpriteInstance* sprite = arena_get(&dim->sprites, hit.sprite_id)) target.center = sprite->origin;
    }
    constexpr float erase_radius_pixels = 2.5f;
    const u32 removed = dimension_erase_pixels(dim, target, erase_radius_pixels);
    NetPaintPixel operation{};
    operation.chunk = target.center.chunk;
    operation.local = target.center.local;
    operation.normal = target.normal;
    operation.mesh_id = target.mesh_id;
    operation.sprite_id = target.sprite_id;
    operation.sprite_pixel_x = target.sprite_pixel_x;
    operation.sprite_pixel_y = target.sprite_pixel_y;
    operation.erase_radius_pixels = erase_radius_pixels;
    net_send_paint_pixel(&app->net, operation);
    if (removed > 0) mark_profile_dirty(app);
    return removed > 0;
}

bool demo_erase_at_view(DemoApp* app, const CameraView& view) {
    if (!app) return false;
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    PlayerEntity* player = dim ? local_player(app, dim) : nullptr;
    if (!dim || !player) return false;
    const WorldPos ray_start = worldpos_offset(view.anchor, {0.0f, view.eye_height, 0.0f}, dim->chunk_size_m);
    const Vector3 ray_dir = forward_from_angles(view.yaw, view.pitch);
    const WorldRayHit hit = raycast_world(app, dim, player, ray_start, ray_dir, paint_max_ray_dist_m);
    return erase_targeted_pixels(app, dim, ray_start, hit);
}

bool demo_paint_at_view(DemoApp* app, const CameraView& view, Color color) {
    if (!app) return false;
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    PlayerEntity* player = dim ? local_player(app, dim) : nullptr;
    if (!dim || !player) return false;
    const WorldPos ray_start = worldpos_offset(view.anchor, {0.0f, view.eye_height, 0.0f}, dim->chunk_size_m);
    const Vector3 ray_dir = forward_from_angles(view.yaw, view.pitch);
    const WorldRayHit hit = raycast_world(app, dim, player, ray_start, ray_dir, paint_max_ray_dist_m);
    return paint_targeted_pixel(app, dim, ray_start, hit, color);
}

static void update_player_aim_ray(DemoApp* app, Dimension* dim, PlayerEntity* player) {
    if (!app || !dim || !player) return;

    player->aim_ray_active = app->mouse_captured && (app->frame_input.mouse_left_down || app->frame_input.mouse_right_down);
    if (!player->aim_ray_active) {
        app->last_drag_pixel_valid = false;
        return;
    }

    const CameraView view = make_player_camera_view(app, dim, player);
    const WorldPos ray_start = worldpos_offset(view.anchor, {0.0f, view.eye_height, 0.0f}, dim->chunk_size_m);
    const Vector3 ray_dir = forward_from_angles(view.yaw, view.pitch);
    const WorldRayHit hit = raycast_world(app, dim, player, ray_start, ray_dir, paint_max_ray_dist_m);

    player->aim_ray_start = ray_start;
    player->aim_ray_end = worldpos_offset(ray_start, ray_dir * hit.distance, dim->chunk_size_m);
    canonicalize(&player->aim_ray_end, dim->chunk_size_m);
    if (app->frame_input.mouse_right_down) erase_targeted_pixels(app, dim, ray_start, hit);
    else paint_targeted_pixel(app, dim, ray_start, hit, player->color);
}

static void update_cave_emissive_animation(DemoApp* app, Dimension* dim, float dt) {
    if (!app || !dim || !is_cave_world(app->world_name)) return;
    app->cave_animation_time += dt;
    const float t = app->cave_animation_time;
    const Vector3 offsets[] = {
        {std::sin(t * 0.73f) * 0.55f, std::sin(t * 0.91f) * 0.45f, std::cos(t * 0.57f) * 0.65f},
        // Keep the cyan cube's near face clear of the arch-right pillar at
        // x=13. The previous 0.75 m swing let its 0.5 m half-width cross the
        // pillar even though the source itself is non-colliding geometry.
        {std::sin(t * 0.61f + 1.7f) * 0.40f, std::sin(t * 1.07f + 0.8f) * 0.65f, std::cos(t * 0.83f) * 0.55f},
        {std::sin(t * 0.49f + 3.1f) * 1.20f, std::sin(t * 0.79f + 2.4f) * 0.50f, std::cos(t * 0.67f + 1.2f) * 0.70f},
    };

    for (u32 i = 0; i < app->cave_emissive_mesh_ids.size(); ++i) {
        MeshInstance* mesh = arena_get(&dim->meshes, app->cave_emissive_mesh_ids[i]);
        if (!mesh) continue;
        const WorldPos base = demo_pos(app->dimension_id, app->cave_emissive_base_positions[i], dim->chunk_size_m);
        const WorldPos animated = demo_pos(
            app->dimension_id,
            app->cave_emissive_base_positions[i] + offsets[i],
            dim->chunk_size_m);
        // The mesh stays registered in its original chunk; keeping the animation
        // inside that chunk avoids stale chunk references in the current world API.
        if (chunk_equal(base.chunk, animated.chunk) &&
            (mesh->origin.local.x != animated.local.x ||
             mesh->origin.local.y != animated.local.y ||
             mesh->origin.local.z != animated.local.z)) {
            mesh->origin = animated;
        }
    }
}

static void fixed_update(DemoApp* app, Dimension* dim, float dt) {
    update_cave_emissive_animation(app, dim, dt);
    PlayerEntity* player = local_player(app, dim);
    if (!player) return;
    PlayerControllerInput input{};
    input.move = app->input.move;
    input.jump_pressed = app->input.jump_pressed;
    input.jump_held = app->input.jump_held;
    input.sprint = app->input.sprint;
    input.crouch = app->input.crouch;
    player_controller_step(dim, player, input, dt);
    physics_step(dim, dt);
    app->input.jump_pressed = false;
}

static NetPlayerState make_net_player_state(DemoApp* app, Dimension* dim, const PlayerEntity* player) {
    NetPlayerState state{};
    if (!dim || !player) return state;

    const WorldPos feet = player_feet_pos(dim, player);
    state.peer_id = app->net.local_peer_id;
    state.player_id = app->local_player_id;
    state.dimension_id = feet.dimension;
    state.chunk = feet.chunk;
    state.local = feet.local;
    state.yaw = player->yaw;
    state.pitch = player->pitch;
    state.body_radius = player->body_radius;
    state.current_height = player->current_height;
    state.color = player->color;
    std::snprintf(state.name, sizeof(state.name), "%s", player->name);
    state.aim_ray_active = player->aim_ray_active;
    state.aim_ray_start_chunk = player->aim_ray_start.chunk;
    state.aim_ray_start_local = player->aim_ray_start.local;
    state.aim_ray_end_chunk = player->aim_ray_end.chunk;
    state.aim_ray_end_local = player->aim_ray_end.local;
    return state;
}

static SavedPlayerState saved_state_from_net(const NetPlayerState& state) {
    SavedPlayerState saved{};
    saved.valid = true;
    saved.peer_id = state.peer_id;
    copy_text(saved.name, sizeof(saved.name), state.name);
    saved.color = state.color;
    saved.chunk = state.chunk;
    saved.local = state.local;
    saved.yaw = state.yaw;
    saved.pitch = state.pitch;
    saved.body_radius = state.body_radius;
    saved.current_height = state.current_height;
    return saved;
}

static NetPlayerState net_state_from_saved(const SavedPlayerState& saved, u32 dimension_id) {
    NetPlayerState state{};
    state.peer_id = saved.peer_id;
    state.dimension_id = dimension_id;
    state.chunk = saved.chunk;
    state.local = saved.local;
    state.yaw = saved.yaw;
    state.pitch = saved.pitch;
    state.body_radius = saved.body_radius;
    state.current_height = saved.current_height;
    state.color = saved.color;
    copy_text(state.name, sizeof(state.name), saved.name);
    return state;
}

static void apply_net_player_state_to_local(DemoApp* app, Dimension* dim, PlayerEntity* player, const NetPlayerState& state) {
    SavedPlayerState saved = saved_state_from_net(state);
    saved.valid = true;
    saved.peer_id = local_player_peer_id(app);
    apply_saved_player_state(dim, app->dimension_id, player, saved, false);
}

static void record_current_world_state(DemoApp* app) {
    copy_text(app->profile.player_name, sizeof(app->profile.player_name), app->player_name);
    copy_text(app->profile.session_name, sizeof(app->profile.session_name), app->session_name);
    copy_world_name(app->profile.world_name, sizeof(app->profile.world_name), app->world_name);
    app->profile.player_color = app->player_color;

    if (!app->in_game) return;
    if (app->net.mode == net_client && !app->net.lobby_owner) return;

    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    PlayerEntity* player = local_player(app, dim);
    if (!dim || !player) return;

    bool changed = false;
    SavedSessionState* session = find_session(&app->profile, app->session_name);
    if (!session) {
        session = upsert_session(&app->profile, app->session_name, app->world_name);
        changed = session != nullptr;
    } else if (std::strcmp(session->world_name, app->world_name) != 0) {
        copy_world_name(session->world_name, sizeof(session->world_name), app->world_name);
        changed = true;
    }
    if (!session) return;

    if (session->captured_paint_revision != dim->paint_revision) {
        session->painted_pixels.clear();
        session->painted_pixels.reserve(dim->painted_pixels.size());
        for (u32 i = 0; i < dim->painted_pixels.size(); ++i) {
            const PaintedPixel& pixel = dim->painted_pixels[i];
            SavedPaintPixel saved{};
            saved.chunk = pixel.center.chunk;
            saved.local = pixel.center.local;
            saved.normal = pixel.normal;
            saved.tangent = pixel.tangent;
            saved.color = pixel.color;
            saved.quad_offset = pixel.quad_offset;
            saved.quad_half_size = pixel.quad_half_size;
            saved.mesh_id = pixel.mesh_id;
            saved.sprite_id = pixel.sprite_id;
            saved.sprite_pixel_x = pixel.sprite_pixel_x;
            saved.sprite_pixel_y = pixel.sprite_pixel_y;
            session->painted_pixels.push_back(saved);
        }
        session->captured_paint_revision = dim->paint_revision;
        ++app->debug_profile_paint_snapshots;
        changed = true;
    }

    NetPlayerState local = make_net_player_state(app, dim, player);
    local.peer_id = local_player_peer_id(app);
    changed = upsert_player_state(session, saved_state_from_net(local)) || changed;

    for (u32 slot = 0; slot < max_players; ++slot) {
        const u64 peer_id = app->remote_peer_ids[slot];
        const u32 player_id = app->remote_player_ids[slot];
        if (!peer_id || !id_valid(player_id)) continue;
        const PlayerEntity* remote = arena_get(&dim->players, player_id);
        if (!remote) continue;
        NetPlayerState remote_state = make_net_player_state(app, dim, remote);
        remote_state.peer_id = peer_id;
        changed = upsert_player_state(session, saved_state_from_net(remote_state)) || changed;
    }

    if (changed) mark_profile_dirty(app);
}

static void autosave_profile_if_needed(DemoApp* app) {
    collect_profile_save_failure(app);
    if (!app->profile_dirty) return;
    constexpr double autosave_debounce_s = 10.0;
    if (GetTime() - app->last_profile_save_time < autosave_debounce_s) return;
    queue_profile_save(app);
}

static u32 find_remote_slot(DemoApp* app, u64 peer_id) {
    for (u32 i = 0; i < max_players; ++i) {
        if (app->remote_peer_ids[i] == peer_id) return i;
    }
    return invalid_id;
}

static u32 acquire_remote_slot(DemoApp* app, u64 peer_id) {
    const u32 existing = find_remote_slot(app, peer_id);
    if (id_valid(existing)) return existing;

    for (u32 i = 0; i < max_players; ++i) {
        if (app->remote_peer_ids[i] == 0) {
            app->remote_peer_ids[i] = peer_id;
            app->remote_player_ids[i] = invalid_id;
            return i;
        }
    }
    return invalid_id;
}

static bool restore_already_sent(const DemoApp* app, u64 peer_id) {
    for (u64 sent : app->restore_sent_peer_ids) {
        if (sent == peer_id) return true;
    }
    return false;
}

static void remember_restore_sent(DemoApp* app, u64 peer_id) {
    if (!peer_id || restore_already_sent(app, peer_id)) return;
    for (u64& sent : app->restore_sent_peer_ids) {
        if (sent == 0) {
            sent = peer_id;
            return;
        }
    }
}

static void forget_restore_sent(DemoApp* app, u64 peer_id) {
    for (u64& sent : app->restore_sent_peer_ids) {
        if (sent == peer_id) sent = 0;
    }
}

static void send_saved_restore_if_needed(DemoApp* app, u64 peer_id) {
    if (!app->net.lobby_owner || !peer_id || restore_already_sent(app, peer_id)) return;
    const SavedPlayerState* saved = find_current_session_player_state(app, peer_id);
    NetPlayerState restore{};
    if (saved) {
        restore = net_state_from_saved(*saved, app->dimension_id);
    } else {
        bool found_current = false;
        for (u32 i = 0; i < app->net.peer_count; ++i) {
            if (app->net.remote_player_valid[i] && app->net.remote_players[i].peer_id == peer_id) {
                restore = app->net.remote_players[i];
                found_current = true;
                break;
            }
        }
        if (!found_current) return;
    }
    net_send_player_restore(&app->net, peer_id, restore);
    remember_restore_sent(app, peer_id);
}

static bool paint_history_sent(const DemoApp* app, u64 peer_id) {
    for (u64 sent : app->paint_sync_sent_peer_ids) if (sent == peer_id) return true;
    return false;
}

static void send_paint_history_if_needed(DemoApp* app, const Dimension* dim, u64 peer_id) {
    if (!app || !dim || !app->net.lobby_owner || !peer_id || paint_history_sent(app, peer_id)) return;
    for (u32 i = 0; i < dim->painted_pixels.size(); ++i) {
        const PaintedPixel& pixel = dim->painted_pixels[i];
        NetPaintPixel net_pixel{};
        net_pixel.chunk = pixel.center.chunk;
        net_pixel.local = pixel.center.local;
        net_pixel.normal = pixel.normal;
        net_pixel.tangent = pixel.tangent;
        net_pixel.color = pixel.color;
        net_pixel.quad_offset = pixel.quad_offset;
        net_pixel.quad_half_size = pixel.quad_half_size;
        net_pixel.mesh_id = pixel.mesh_id;
        net_pixel.sprite_id = pixel.sprite_id;
        net_pixel.sprite_pixel_x = pixel.sprite_pixel_x;
        net_pixel.sprite_pixel_y = pixel.sprite_pixel_y;
        net_send_paint_pixel(&app->net, net_pixel);
    }
    NetPaintPixel complete{};
    complete.erase_radius_pixels = -1.0f;
    net_send_paint_pixel(&app->net, complete);
    for (u64& sent : app->paint_sync_sent_peer_ids) {
        if (sent == 0) {
            sent = peer_id;
            break;
        }
    }
}

static bool net_peer_present(const NetSession* net, u64 peer_id) {
    for (u32 i = 0; i < net->peer_count; ++i) {
        if (net->peer_ids[i] == peer_id) return true;
    }
    return false;
}

static Vector3 worldpos_to_meters(WorldPos pos, float chunk_size) {
    return {
        static_cast<float>(world_axis_meters(pos.chunk.x, pos.local.x, chunk_size)),
        static_cast<float>(world_axis_meters(pos.chunk.y, pos.local.y, chunk_size)),
        static_cast<float>(world_axis_meters(pos.chunk.z, pos.local.z, chunk_size)),
    };
}

static void copy_current_view_to_clipboard(const Dimension* dim, const CameraView& view) {
    const WorldPos eye_pos = worldpos_offset(view.anchor, {0.0f, view.eye_height, 0.0f}, dim->chunk_size_m);
    const Vector3 eye = worldpos_to_meters(eye_pos, dim->chunk_size_m);
    const Vector3 target = eye + forward_from_angles(view.yaw, view.pitch) * 8.0f;
    const Vector3 feet = worldpos_to_meters(view.anchor, dim->chunk_size_m);

    char text[512]{};
    std::snprintf(
        text,
        sizeof(text),
        "EdgeCompareView{\"clipboard\", {%.4ff, %.4ff, %.4ff}, {%.4ff, %.4ff, %.4ff}}\n"
        "feet={%.4ff, %.4ff, %.4ff} yaw=%.6ff pitch=%.6ff",
        eye.x,
        eye.y,
        eye.z,
        target.x,
        target.y,
        target.z,
        feet.x,
        feet.y,
        feet.z,
        view.yaw,
        view.pitch);
    SetClipboardText(text);
    TraceLog(LOG_INFO, "Copied camera view to clipboard");
}

static void remove_remote_player(Dimension* dim, u32 player_id) {
    PlayerEntity* player = arena_get(&dim->players, player_id);
    if (!player) return;
    arena_remove(&dim->physics.masses, player->bottom_mass);
    arena_remove(&dim->physics.masses, player->top_mass);
    arena_remove(&dim->physics.links, player->body_link);
    arena_remove(&dim->players, player_id);
}

static void sync_remote_players(DemoApp* app, Dimension* dim) {
    if (!dim) return;

    for (u32 slot = 0; slot < max_players; ++slot) {
        const u64 peer_id = app->remote_peer_ids[slot];
        if (!peer_id || net_peer_present(&app->net, peer_id)) continue;
        if (id_valid(app->remote_player_ids[slot])) {
            remove_remote_player(dim, app->remote_player_ids[slot]);
        }
        forget_restore_sent(app, peer_id);
        for (u64& sent : app->paint_sync_sent_peer_ids) if (sent == peer_id) sent = 0;
        app->remote_peer_ids[slot] = 0;
        app->remote_player_ids[slot] = invalid_id;
    }

    for (u32 net_idx = 0; net_idx < app->net.peer_count; ++net_idx) {
        if (!app->net.remote_player_valid[net_idx]) continue;
        const NetPlayerState& state = app->net.remote_players[net_idx];
        if (!state.peer_id || state.peer_id == app->net.local_peer_id) continue;

        const u32 slot = acquire_remote_slot(app, state.peer_id);
        if (!id_valid(slot)) continue;

        WorldPos feet{};
        feet.dimension = app->dimension_id;
        feet.chunk = state.chunk;
        feet.local = state.local;
        canonicalize(&feet, dim->chunk_size_m);

        const SavedPlayerState* saved = find_current_session_player_state(app, state.peer_id);
        WorldPos spawn_feet = feet;
        if (saved && !restore_already_sent(app, state.peer_id)) {
            spawn_feet = saved_player_feet_pos(app->dimension_id, *saved);
            canonicalize(&spawn_feet, dim->chunk_size_m);
        }

        if (!id_valid(app->remote_player_ids[slot])) {
            const char* name = state.name[0] ? state.name : "peer";
            app->remote_player_ids[slot] = dimension_add_player(dim, name, state.color, spawn_feet, false);
        }

        PlayerEntity* remote = arena_get(&dim->players, app->remote_player_ids[slot]);
        if (!remote) continue;
        std::snprintf(remote->name, sizeof(remote->name), "%s", state.name[0] ? state.name : "peer");
        remote->color = state.color;
        remote->yaw = state.yaw;
        remote->pitch = state.pitch;
        remote->body_radius = state.body_radius > 0.0f ? state.body_radius : player_radius_m;
        remote->current_height = state.current_height > 0.0f ? state.current_height : player_stand_height_m;
        remote->eye_height = remote->current_height < player_stand_height_m - 0.01f ? player_crouch_eye_m : player_stand_eye_m;
        remote->is_crouching = remote->current_height < player_stand_height_m - 0.01f;
        remote->connected = true;
        remote->local = false;
        remote->aim_ray_active = state.aim_ray_active;
        remote->aim_ray_start = {app->dimension_id, state.aim_ray_start_chunk, state.aim_ray_start_local};
        remote->aim_ray_end = {app->dimension_id, state.aim_ray_end_chunk, state.aim_ray_end_local};
        canonicalize(&remote->aim_ray_start, dim->chunk_size_m);
        canonicalize(&remote->aim_ray_end, dim->chunk_size_m);
        player_sync_masses_to_pose(dim, remote, feet);

        if (saved && !restore_already_sent(app, state.peer_id)) {
            apply_saved_player_state(dim, app->dimension_id, remote, *saved, true);
        }
        send_saved_restore_if_needed(app, state.peer_id);
        send_paint_history_if_needed(app, dim, state.peer_id);
    }
}

static bool only_local_player(const DemoApp* app) {
    return app->net.peer_count == 0;
}

static void pause_game(DemoApp* app) {
    app->paused = true;
    app->mouse_captured = false;
    app->input = {};
    app->dragged_pause_control = pause_control_none;
    EnableCursor();
}

static void resume_game(DemoApp* app) {
    app->paused = false;
    app->mouse_captured = true;
    app->blocking_screen_cursor_released = false;
    app->input = {};
    app->dragged_pause_control = pause_control_none;
    DisableCursor();
}

static void exit_to_first_menu(DemoApp* app) {
    record_current_world_state(app);
    save_profile(app);
    net_leave(&app->net);
    reset_remote_players(app);
    app->in_game = false;
    app->paused = false;
    app->mouse_captured = false;
    app->blocking_screen_cursor_released = false;
    app->joining_screen_active = false;
    app->input = {};
    app->fixed_accum = 0.0f;
    app->dragged_pause_control = pause_control_none;
    app->dragged_menu_control = menu_control_none;
    app->active_menu_field = menu_input_player;
    app->sessions_open = false;
    app->worlds_open = false;
    EnableCursor();
    ShowCursor();
}

static CameraView make_player_camera_view(DemoApp* app, Dimension* dim, const PlayerEntity* player) {
    CameraView view{};
    view.anchor = player_feet_pos(dim, player);
    view.eye_height = player->eye_height + player->camera_y_offset;
    view.yaw = player->yaw;
    view.pitch = player->pitch;
    return view;
}

static void draw_game_scene(DemoApp* app, Dimension* dim, PlayerEntity* player, bool allow_copy_view, bool present_to_screen) {
    CameraView view = make_player_camera_view(app, dim, player);
    if (allow_copy_view && app->frame_input.o_pressed) copy_current_view_to_clipboard(dim, view);
    renderer_ensure_target(&app->renderer);
    if (present_to_screen) {
        renderer_render_dimension_to_target(&app->renderer, dim, view, app->local_player_id);
        BeginDrawing();
        renderer_draw_target_to_screen(&app->renderer);
        if (app->profile.show_fps) DrawFPS(10, 10);
        EndDrawing();
    } else {
        renderer_render_dimension_to_target(&app->renderer, dim, view, app->local_player_id);
    }
}

static void draw_pause(DemoApp* app, Dimension* dim, PlayerEntity* player) {
    draw_game_scene(app, dim, player, false, false);
    demo_draw_pause_overlay_screen(PauseScreen{
        &app->renderer,
        static_cast<int>(floorf(app->renderer.fov + 0.5f)),
        app->renderer.scale_power,
        dim ? dim->render_radius_chunks : 6,
        visible_window_ready() ? IsWindowFullscreen() : app->profile.fullscreen,
        app->profile.show_fps,
        app->renderer.lighting
    });
}

static bool screen_point_in_rect(Vector2 p, Rectangle r) {
    return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
}

static void draw_centered_screen_text(DemoApp* app, const char* text, float y, float size, Color color) {
    Font font = app->renderer.font_ready ? app->renderer.font : GetFontDefault();
    const Vector2 measured = MeasureTextEx(font, text, size, 1.0f);
    DrawTextEx(font, text, {static_cast<float>(GetScreenWidth()) * 0.5f - measured.x * 0.5f, y}, size, 1.0f, color);
}

static Rectangle modal_leave_rect() {
    const float w = 180.0f;
    const float h = 52.0f;
    return {
        static_cast<float>(GetScreenWidth()) * 0.5f - w * 0.5f,
        static_cast<float>(GetScreenHeight()) * 0.5f + 50.0f,
        w,
        h
    };
}

static void draw_blocking_status_screen(DemoApp* app, const char* title, const char* subtitle, bool leave_button) {
    BeginDrawing();
    ClearBackground(BLACK);
    const float center_y = static_cast<float>(GetScreenHeight()) * 0.5f;
    draw_centered_screen_text(app, title, center_y - 80.0f, 42.0f, WHITE);
    if (subtitle && subtitle[0]) {
        draw_centered_screen_text(app, subtitle, center_y - 22.0f, 22.0f, Color{158, 176, 198, 255});
    }
    if (leave_button) {
        const Rectangle button = modal_leave_rect();
        const bool hover = screen_point_in_rect(GetMousePosition(), button);
        DrawRectangleRounded(button, 0.10f, 8, hover ? Color{38, 45, 58, 255} : Color{20, 24, 32, 255});
        DrawRectangleRoundedLinesEx(button, 0.10f, 8, 2.0f, WHITE);
        draw_centered_screen_text(app, "leave", button.y + 12.0f, 26.0f, WHITE);
    }
    EndDrawing();
}

static void release_cursor_for_blocking_screen(DemoApp* app) {
    app->mouse_captured = false;
    app->paused = false;
    app->input = {};

    if (!app->blocking_screen_cursor_released) {
        EnableCursor();
        ShowCursor();
        app->blocking_screen_cursor_released = true;
    } else {
        ShowCursor();
    }
}

static bool update_host_left_screen(DemoApp* app) {
    if (!app->net.host_left) return false;

    release_cursor_for_blocking_screen(app);

    if ((app->frame_input.mouse_left_pressed && screen_point_in_rect(GetMousePosition(), modal_leave_rect())) ||
        app->frame_input.enter_pressed ||
        app->frame_input.escape_pressed) {
        exit_to_first_menu(app);
        demo_draw_menu(app);
        return true;
    }

    draw_blocking_status_screen(app, "Host left", "The session ended.", true);
    return true;
}

static bool update_joining_screen(DemoApp* app, Dimension* dim, PlayerEntity* player) {
    if (!(app->net.mode == net_client && app->joining_screen_active)) return false;

    release_cursor_for_blocking_screen(app);

    update_network_state(app, dim, player);
    dim = world_get_dimension(&app->world, app->dimension_id);
    player = dim ? local_player(app, dim) : nullptr;
    if (dim) apply_received_paint_pixels(app, dim);
    if (app->net.in_lobby && dim && player && app->joining_restore_applied && app->joining_paint_history_complete) {
        demo_update_landscape_streaming(app, dim, player);
        if (GetTime() - app->joining_started >= 0.35) {
            app->joining_screen_active = false;
            app->mouse_captured = true;
            app->blocking_screen_cursor_released = false;
            DisableCursor();
            return false;
        }
    }
    draw_blocking_status_screen(app, "Joining", app->net.status, false);
    return true;
}

static void update_pause_menu(DemoApp* app, Dimension* dim) {
    if (app->frame_input.escape_pressed) {
        resume_game(app);
        return;
    }

    if (app->frame_input.mouse_left_pressed) {
        const PauseHit hit = demo_pause_hit_test(GetMousePosition());
        app->dragged_pause_control = pause_control_none;
        if (hit.control == pause_control_continue) {
            resume_game(app);
            return;
        }
        if (hit.control == pause_control_first_menu) {
            exit_to_first_menu(app);
            return;
        }
        if (hit.control == pause_control_fullscreen) {
            toggle_fullscreen_setting(app);
            return;
        }
        if (hit.control == pause_control_fps_counter) {
            set_fps_counter_enabled(app, !app->profile.show_fps, true);
            return;
        }
        if (hit.control == pause_control_lighting_enabled) {
            app->renderer.lighting.enabled = !app->renderer.lighting.enabled;
            mark_profile_dirty(app);
            return;
        }
        if (hit.control == pause_control_lighting_jitter) {
            app->renderer.lighting.jitter = !app->renderer.lighting.jitter;
            mark_profile_dirty(app);
            return;
        }
        if (hit.control == pause_control_lighting_corner_merge) {
            app->renderer.lighting.corner_merge = !app->renderer.lighting.corner_merge;
            mark_profile_dirty(app);
            return;
        }
        if (hit.control == pause_control_fov || hit.control == pause_control_scale ||
            hit.control == pause_control_render_radius ||
            hit.control == pause_control_lighting_probe_levels ||
            hit.control == pause_control_lighting_iterations ||
            hit.control == pause_control_lighting_indirect_samples ||
            hit.control == pause_control_lighting_shadow_samples ||
            hit.control == pause_control_lighting_temporal_frames ||
            hit.control == pause_control_lighting_radius) {
            app->dragged_pause_control = hit.control;
        }
    }

    if (app->frame_input.mouse_left_down) {
        if (app->dragged_pause_control == pause_control_fov) {
            const int fov = demo_pause_value_from_mouse(pause_control_fov, GetMousePosition());
            if (static_cast<int>(floorf(app->renderer.fov + 0.5f)) != fov) {
                app->renderer.fov = static_cast<float>(fov);
                mark_profile_dirty(app);
            }
        } else if (app->dragged_pause_control == pause_control_scale) {
            const int scale_power = demo_pause_value_from_mouse(pause_control_scale, GetMousePosition());
            if (app->renderer.scale_power != scale_power) {
                app->renderer.scale_power = scale_power;
                mark_profile_dirty(app);
            }
        } else if (app->dragged_pause_control == pause_control_render_radius) {
            const int radius = demo_pause_value_from_mouse(pause_control_render_radius, GetMousePosition());
            set_dimension_render_radius(app, dim, radius, true);
        } else if (app->dragged_pause_control == pause_control_lighting_probe_levels) {
            const int value = demo_pause_value_from_mouse(pause_control_lighting_probe_levels, GetMousePosition());
            if (app->renderer.lighting.probe_extra_levels != value) {
                app->renderer.lighting.probe_extra_levels = value;
                mark_profile_dirty(app);
            }
        } else if (app->dragged_pause_control == pause_control_lighting_iterations) {
            const int value = demo_pause_value_from_mouse(pause_control_lighting_iterations, GetMousePosition());
            if (app->renderer.lighting.cascade_iterations != value) {
                app->renderer.lighting.cascade_iterations = value;
                mark_profile_dirty(app);
            }
        } else if (app->dragged_pause_control == pause_control_lighting_indirect_samples) {
            const int value = demo_pause_value_from_mouse(pause_control_lighting_indirect_samples, GetMousePosition());
            if (app->renderer.lighting.indirect_samples != value) {
                app->renderer.lighting.indirect_samples = value;
                mark_profile_dirty(app);
            }
        } else if (app->dragged_pause_control == pause_control_lighting_shadow_samples) {
            const int value = demo_pause_value_from_mouse(pause_control_lighting_shadow_samples, GetMousePosition());
            if (app->renderer.lighting.shadow_samples != value) {
                app->renderer.lighting.shadow_samples = value;
                mark_profile_dirty(app);
            }
        } else if (app->dragged_pause_control == pause_control_lighting_temporal_frames) {
            const int value = demo_pause_value_from_mouse(pause_control_lighting_temporal_frames, GetMousePosition());
            if (app->renderer.lighting.temporal_frames != value) {
                app->renderer.lighting.temporal_frames = value;
                mark_profile_dirty(app);
            }
        } else if (app->dragged_pause_control == pause_control_lighting_radius) {
            const int value = demo_pause_value_from_mouse(pause_control_lighting_radius, GetMousePosition());
            if (app->renderer.lighting.lighting_radius_chunks != value) {
                app->renderer.lighting.lighting_radius_chunks = value;
                mark_profile_dirty(app);
            }
        }
    }

    if (app->frame_input.mouse_left_released) {
        app->dragged_pause_control = pause_control_none;
    }
}

static void tick_game_simulation(DemoApp* app, Dimension* dim, float frame_time) {
    app->fixed_accum += fminf(frame_time, 0.05f);
    constexpr float fixed_dt = 1.0f / 60.0f;
    u32 steps = 0;
    while (app->fixed_accum >= fixed_dt && steps < 3) {
        fixed_update(app, dim, fixed_dt);
        app->fixed_accum -= fixed_dt;
        ++steps;
    }
    if (app->fixed_accum >= fixed_dt) {
        app->fixed_accum = 0.0f;
    }
}

static bool sync_session_from_network(DemoApp* app, Dimension* dim, PlayerEntity* player) {
    if (!app->net.in_lobby) return false;

    const bool entered_lobby = app->net.just_entered_lobby;
    const bool session_changed = app->net.session_name[0] &&
        std::strcmp(app->session_name, app->net.session_name) != 0;
    const bool world_changed = app->net.world_name[0] &&
        std::strcmp(canonical_world_name(app->world_name), canonical_world_name(app->net.world_name)) != 0;

    if (session_changed || world_changed) {
        // Capture and durably flush the old world before changing the keys used to find
        // its session or rebuilding its dimension. Transitions already obscure gameplay,
        // so this is the safe place for the rare synchronous persistence barrier.
        record_current_world_state(app);
        save_profile(app);
    }

    if (session_changed) {
        copy_text(app->session_name, sizeof(app->session_name), app->net.session_name);
        copy_text(app->profile.session_name, sizeof(app->profile.session_name), app->session_name);
        app->restore_sent_peer_ids.fill(0);
        mark_profile_dirty(app);
    }

    bool rebuilt_world = false;
    if (world_changed) {
        copy_world_name(app->world_name, sizeof(app->world_name), app->net.world_name);
        copy_world_name(app->profile.world_name, sizeof(app->profile.world_name), app->world_name);
        app->restore_sent_peer_ids.fill(0);
        mark_profile_dirty(app);
        demo_generate_world(app);
        dim = world_get_dimension(&app->world, app->dimension_id);
        player = local_player(app, dim);
        rebuilt_world = true;
    }

    if (session_changed || world_changed) {
        upsert_session(&app->profile, app->session_name, app->world_name);
    }

    if (session_changed && !world_changed && app->net.mode == net_client && !app->net.lobby_owner) {
        apply_current_session_local_spawn(app, dim, player);
    }

    if (entered_lobby || session_changed || world_changed) {
        app->net.last_send_time = 0.0;
        app->net.last_reliable_send_time = -1000.0;
    }

    if (entered_lobby) {
        app->restore_sent_peer_ids.fill(0);
        app->net.just_entered_lobby = false;
    }
    return rebuilt_world;
}

static bool update_network_state(DemoApp* app, Dimension* dim, PlayerEntity* player) {
    net_set_local_player(&app->net, make_net_player_state(app, dim, player));
    net_update(&app->net);
    const bool rebuilt_world = sync_session_from_network(app, dim, player);
    if (rebuilt_world) {
        dim = world_get_dimension(&app->world, app->dimension_id);
        player = local_player(app, dim);
        if (!dim || !player) return true;
        net_set_local_player(&app->net, make_net_player_state(app, dim, player));
    }

    NetPlayerState restore{};
    if (net_take_player_restore(&app->net, &restore)) {
        apply_net_player_state_to_local(app, dim, player, restore);
        app->joining_restore_applied = true;
    }
    sync_remote_players(app, dim);
    record_current_world_state(app);
    autosave_profile_if_needed(app);
    return rebuilt_world;
}

static void update_game(DemoApp* app) {
    Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
    PlayerEntity* player = local_player(app, dim);
    if (!dim || !player) return;

    if (update_host_left_screen(app)) return;
    if (update_joining_screen(app, dim, player)) return;
    if (!app->paused && app->frame_input.escape_pressed) {
        pause_game(app);
        if (only_local_player(app)) {
            app->fixed_accum = 0.0f;
        } else {
            tick_game_simulation(app, dim, GetFrameTime());
        }
        demo_update_landscape_streaming(app, dim, player);
        player->aim_ray_active = false;
        if (update_network_state(app, dim, player)) {
            dim = world_get_dimension(&app->world, app->dimension_id);
            player = local_player(app, dim);
            if (!dim || !player) return;
        }
        if (update_host_left_screen(app)) return;
        draw_pause(app, dim, player);
        return;
    } else if (app->paused) {
        update_pause_menu(app, dim);
        if (!app->in_game) return;

        app->input = {};
        if (app->paused) {
            if (only_local_player(app)) {
                app->fixed_accum = 0.0f;
            } else {
                tick_game_simulation(app, dim, GetFrameTime());
            }
            demo_update_landscape_streaming(app, dim, player);
            player->aim_ray_active = false;
            if (update_network_state(app, dim, player)) {
                dim = world_get_dimension(&app->world, app->dimension_id);
                player = local_player(app, dim);
                if (!dim || !player) return;
            }
            if (update_host_left_screen(app)) return;
            draw_pause(app, dim, player);
            return;
        }
    }

    if (app->frame_input.plus_pressed) {
        const int before = app->renderer.scale_power;
        renderer_change_scale(&app->renderer, 1);
        if (app->renderer.scale_power != before) mark_profile_dirty(app);
    }
    if (app->frame_input.minus_pressed) {
        const int before = app->renderer.scale_power;
        renderer_change_scale(&app->renderer, -1);
        if (app->renderer.scale_power != before) mark_profile_dirty(app);
    }
    if (app->frame_input.f3_pressed) app->renderer.draw_physics_debug = !app->renderer.draw_physics_debug;
    if (app->frame_input.p_pressed) {
        renderer_toggle_pathtrace_comparison(&app->renderer);
    }
    if (app->frame_input.r_pressed) {
        respawn_player_at_default(app, dim, player);
        app->fixed_accum = 0.0f;
        app->input = {};
    }
    if (app->frame_input.c_pressed && app->net.in_lobby) net_copy_lobby_to_clipboard(&app->net);

    collect_input(app, player);
    tick_game_simulation(app, dim, GetFrameTime());
    demo_update_landscape_streaming(app, dim, player);
    update_player_aim_ray(app, dim, player);
    if (update_network_state(app, dim, player)) {
        dim = world_get_dimension(&app->world, app->dimension_id);
        player = local_player(app, dim);
        if (!dim || !player) return;
    }
    apply_received_paint_pixels(app, dim);
    if (update_host_left_screen(app)) return;

    draw_game_scene(app, dim, player, true, true);
}

bool demo_update_and_draw(DemoApp* app) {
    if (WindowShouldClose()) return false;
    capture_frame_input(app);
    if (app->frame_input.f11_pressed) toggle_fullscreen_setting(app);
    if (!app->in_game && app->frame_input.escape_pressed) return false;
    if (!app->in_game) {
        update_menu(app);
        autosave_profile_if_needed(app);
        if (!app->in_game) demo_draw_menu(app);
    } else {
        update_game(app);
    }
    return true;
}

int demo_run_steam_host_smoke(double timeout_s, double hold_s) {
    write_smoke_log("STEAM_HOST_START timeout=%.2f hold=%.2f", timeout_s, hold_s);
#if defined(OL_DISABLE_RAYLIB_LOGGING)
    SetTraceLogLevel(LOG_NONE);
#else
    SetTraceLogLevel(LOG_WARNING);
#endif
    SetConfigFlags(FLAG_WINDOW_HIDDEN);

    auto* app = new DemoApp();
    demo_init(app);
    net_host(&app->net, app->session_name, app->world_name);
    demo_generate_world(app);

    const double start = GetTime();
    while (GetTime() - start < timeout_s) {
        Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
        PlayerEntity* player = dim ? local_player(app, dim) : nullptr;
        if (dim && player) {
            net_set_local_player(&app->net, make_net_player_state(app, dim, player));
        }
        net_update(&app->net);
        if (app->net.in_lobby && app->net.lobby_id) {
            write_smoke_lobby_file(app->net.lobby_id);
            write_smoke_log("STEAM_HOST_OK lobby=%llu status=\"%s\"",
                static_cast<unsigned long long>(app->net.lobby_id),
                app->net.status);
            const double hold_start = GetTime();
            while (GetTime() - hold_start < hold_s) {
                net_update(&app->net);
                WaitTime(0.05);
            }
            demo_shutdown(app);
            delete app;
            return 0;
        }
        WaitTime(0.05);
    }

    write_smoke_log("STEAM_HOST_FAIL status=\"%s\"", app->net.status);
    demo_shutdown(app);
    delete app;
    return 1;
}

int demo_run_steam_join_smoke(const char* lobby_id, double timeout_s) {
    write_smoke_log("STEAM_JOIN_START lobby=%s timeout=%.2f", lobby_id ? lobby_id : "", timeout_s);
#if defined(OL_DISABLE_RAYLIB_LOGGING)
    SetTraceLogLevel(LOG_NONE);
#else
    SetTraceLogLevel(LOG_WARNING);
#endif
    SetConfigFlags(FLAG_WINDOW_HIDDEN);

    auto* app = new DemoApp();
    demo_init(app);
    SetClipboardText(lobby_id ? lobby_id : "");
    net_join_from_clipboard(&app->net);
    demo_generate_world(app);

    const double start = GetTime();
    while (GetTime() - start < timeout_s) {
        Dimension* dim = world_get_dimension(&app->world, app->dimension_id);
        PlayerEntity* player = dim ? local_player(app, dim) : nullptr;
        if (dim && player) {
            net_set_local_player(&app->net, make_net_player_state(app, dim, player));
        }
        net_update(&app->net);
        if (app->net.in_lobby && app->net.lobby_id) {
            write_smoke_log("STEAM_JOIN_OK lobby=%llu status=\"%s\" peers=%u",
                static_cast<unsigned long long>(app->net.lobby_id),
                app->net.status,
                static_cast<unsigned>(app->net.peer_count));
            demo_shutdown(app);
            delete app;
            return 0;
        }
        WaitTime(0.05);
    }

    write_smoke_log("STEAM_JOIN_FAIL status=\"%s\"", app->net.status);
    demo_shutdown(app);
    delete app;
    return 1;
}

} // namespace ol
