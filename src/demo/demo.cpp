#include "demo/demo.h"
#include "demo/menu.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ol {

static void record_current_world_state(DemoApp* app);
static void save_profile(DemoApp* app);
static CameraView make_player_camera_view(DemoApp* app, Dimension* dim, const PlayerEntity* player);
static bool sync_session_from_network(DemoApp* app, Dimension* dim, PlayerEntity* player);

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
    input.mouse_left_down = left_down;
    input.mouse_left_pressed = IsMouseButtonPressed(MOUSE_LEFT_BUTTON) || (left_down && !app->previous_mouse_left_down);
    input.mouse_left_released = IsMouseButtonReleased(MOUSE_LEFT_BUTTON) || (!left_down && app->previous_mouse_left_down);

    input.tab_pressed = frame_key_pressed(app, KEY_TAB, queued_keys);
    input.backspace_pressed = frame_key_pressed(app, KEY_BACKSPACE, queued_keys);
    input.escape_pressed = frame_key_pressed(app, KEY_ESCAPE, queued_keys);
    input.enter_pressed = frame_key_pressed(app, KEY_ENTER, queued_keys);
    input.plus_pressed = frame_key_pressed(app, KEY_EQUAL, queued_keys) || frame_key_pressed(app, KEY_KP_ADD, queued_keys);
    input.minus_pressed = frame_key_pressed(app, KEY_MINUS, queued_keys) || frame_key_pressed(app, KEY_KP_SUBTRACT, queued_keys);
    input.f3_pressed = frame_key_pressed(app, KEY_F3, queued_keys);
    input.r_pressed = frame_key_pressed(app, KEY_R, queued_keys);
    input.c_pressed = frame_key_pressed(app, KEY_C, queued_keys);
    input.o_pressed = frame_key_pressed(app, KEY_O, queued_keys);
    input.space_pressed = frame_key_pressed(app, KEY_SPACE, queued_keys);

    app->frame_input = input;
    app->previous_mouse_left_down = left_down;

    const int tracked_keys[] = {
        KEY_TAB, KEY_BACKSPACE, KEY_ESCAPE, KEY_ENTER, KEY_EQUAL, KEY_KP_ADD,
        KEY_MINUS, KEY_KP_SUBTRACT, KEY_F3, KEY_R, KEY_C, KEY_O, KEY_SPACE
    };
    for (int key : tracked_keys) update_previous_key(app, key);
}

static constexpr const char* world_playground_name = "playground";
static constexpr const char* world_hills_name = "hills";
static constexpr const char* world_choices[] = {
    world_playground_name,
    world_hills_name,
};
static constexpr u32 world_choice_count = static_cast<u32>(sizeof(world_choices) / sizeof(world_choices[0]));

static const char* canonical_world_name(const char* name) {
    if (name && std::strcmp(name, world_hills_name) == 0) return world_hills_name;
    return world_playground_name;
}

static bool is_landscape_world(const char* name) {
    return std::strcmp(canonical_world_name(name), world_hills_name) == 0;
}

static int default_quality_radius_for_world(const char* name) {
    return is_landscape_world(name) ? 3 : 4;
}

static int clamped_render_radius(int radius) {
    return static_cast<int>(clampf(static_cast<float>(radius), static_cast<float>(pause_render_radius_min), static_cast<float>(pause_render_radius_max)));
}

static void copy_world_name(char* dst, size_t cap, const char* src) {
    copy_text(dst, cap, canonical_world_name(src));
}

static void mark_profile_dirty(DemoApp* app) {
    app->profile_dirty = true;
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

static void upsert_player_state(SavedSessionState* session, const SavedPlayerState& state) {
    if (!session || !state.valid) return;
    if (SavedPlayerState* existing = find_saved_player(session, state.peer_id)) {
        *existing = state;
        return;
    }
    if (session->player_count >= max_players) return;
    session->players[session->player_count++] = state;
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
        return;
    }

    SavedSessionState* current = nullptr;
    char line[512]{};
    while (std::fgets(line, sizeof(line), file)) {
        char* cmd = std::strtok(line, " \t\r\n");
        if (!cmd || cmd[0] == '#') continue;

        if (std::strcmp(cmd, "player") == 0) {
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
    app->profile_dirty = false;
}

static void save_profile(DemoApp* app) {
    copy_text(app->profile.player_name, sizeof(app->profile.player_name), app->player_name);
    copy_text(app->profile.session_name, sizeof(app->profile.session_name), app->session_name);
    copy_world_name(app->profile.world_name, sizeof(app->profile.world_name), app->world_name);
    app->profile.player_color = app->player_color;
    app->profile.fov = static_cast<int>(clampf(floorf(app->renderer.fov + 0.5f), 60.0f, 120.0f));
    app->profile.scale_power = static_cast<int>(clampf(static_cast<float>(app->renderer.scale_power), 0.0f, 4.0f));
    if (app->profile.render_radius_chunks > 0) {
        app->profile.render_radius_chunks = clamped_render_radius(app->profile.render_radius_chunks);
    }

    std::FILE* file = std::fopen("ol_state.txt", "w");
    if (!file) return;

    char encoded[128]{};
    std::fprintf(file, "OL_STATE 1\n");
    encode_text_token(app->profile.player_name, encoded, sizeof(encoded));
    std::fprintf(file, "player %s\n", encoded);
    encode_text_token(app->profile.session_name, encoded, sizeof(encoded));
    std::fprintf(file, "session %s\n", encoded);
    encode_text_token(app->profile.world_name, encoded, sizeof(encoded));
    std::fprintf(file, "world %s\n", encoded);
    std::fprintf(file, "color %u %u %u\n",
        static_cast<unsigned>(app->profile.player_color.r),
        static_cast<unsigned>(app->profile.player_color.g),
        static_cast<unsigned>(app->profile.player_color.b));
    std::fprintf(file, "fov %d\n", app->profile.fov);
    std::fprintf(file, "scale %d\n", app->profile.scale_power);
    if (app->profile.render_radius_chunks > 0) {
        std::fprintf(file, "render_radius %d\n", app->profile.render_radius_chunks);
    }

    for (u32 i = 0; i < app->profile.session_count; ++i) {
        const SavedSessionState& session = app->profile.sessions[i];
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
        std::fprintf(file, "end\n");
    }

    std::fclose(file);
    app->profile_dirty = false;
    app->last_profile_save_time = GetTime();
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
    return demo_pos(dimension_id, {0.0f, 0.0f, 8.0f}, chunk_size);
}

static void reset_remote_players(DemoApp* app) {
    app->remote_peer_ids.fill(0);
    app->remote_player_ids.fill(invalid_id);
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
    mesh.bounds_radius = Vector3Length(size) * 0.5f;
    return dimension_add_mesh(dim, mesh);
}

static void add_static_box(Dimension* dim, u32 dimension_id, u32 cube_geom, const char* name, Vector3 center, Vector3 size, Color color, bool lit = true, bool draw_edges = true) {
    const u32 mesh_id = add_visual_box(dim, dimension_id, cube_geom, name, center, size, color);
    MeshInstance* mesh = arena_get(&dim->meshes, mesh_id);
    if (mesh) {
        mesh->lit = lit;
        mesh->draw_edges = draw_edges;
    }

    BoxCollider box{};
    box.pos = demo_pos(dimension_id, center, dim->chunk_size_m);
    box.half = size * 0.5f;
    box.color = color;
    physics_add_box(&dim->physics, box);
}

static void clear_streamed_chunk_records(DemoApp* app) {
    app->streamed_chunks.clear();
    app->streamed_chunks.reserve(512);
    app->landscape_stream_center_valid = false;
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
    add_streamed_box(app, dim, chunk, "hill house back wall", {base_x, base_y + 1.45f, base_z - 4.05f}, {8.4f, 2.9f, 0.35f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house left wall", {base_x - 4.05f, base_y + 1.45f, base_z}, {0.35f, 2.9f, 8.4f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house right wall", {base_x + 4.05f, base_y + 1.45f, base_z}, {0.35f, 2.9f, 8.4f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house front left", {base_x - 2.75f, base_y + 1.45f, base_z + 4.05f}, {2.6f, 2.9f, 0.35f}, wall, collider, true, true);
    add_streamed_box(app, dim, chunk, "hill house front right", {base_x + 2.75f, base_y + 1.45f, base_z + 4.05f}, {2.6f, 2.9f, 0.35f}, wall, collider, true, true);
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

    if (app->landscape_stream_center_valid && chunk_equal(app->landscape_stream_center, center.chunk)) return;
    app->landscape_stream_center_valid = true;
    app->landscape_stream_center = center.chunk;

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

    for (StreamedWorldChunk& chunk : app->streamed_chunks) {
        if (!chunk.valid) continue;
        const i32 dx = chunk.coord.x - cx;
        const i32 dz = chunk.coord.z - cz;
        const i32 dist_sq = dx * dx + dz * dz;
        if (dist_sq > keep_radius * keep_radius) {
            unload_streamed_chunk(dim, &chunk);
        } else if (dist_sq > collider_radius_sq && chunk.colliders_loaded) {
            unload_streamed_chunk_colliders(dim, &chunk);
        }
    }

    for (i32 dz = -radius; dz <= radius; ++dz) {
        for (i32 dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dz * dz > radius * radius) continue;
            ChunkCoord coord{cx + dx, 0, cz + dz};
            const bool collidable = dx * dx + dz * dz <= collider_radius_sq;
            StreamedWorldChunk* chunk = find_streamed_chunk(app, coord);
            if (!chunk) {
                generate_landscape_chunk(app, dim, coord, collidable);
            } else if (collidable && !chunk->colliders_loaded) {
                unload_streamed_chunk(dim, chunk);
                generate_landscape_chunk(app, dim, coord, true);
            }
        }
    }
}

static void update_landscape_streaming(DemoApp* app, Dimension* dim, const PlayerEntity* player) {
    if (!app || !dim || !player || !app->landscape_streaming) return;
    ensure_landscape_chunks_around(app, dim, player_feet_pos(dim, player));
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

    constexpr float x0 = -1.725f;
    constexpr float x1 = -1.275f;
    constexpr float x2 = 1.275f;
    constexpr float x3 = 1.725f;
    constexpr float y0 = -0.65f;
    constexpr float y1 = 0.35f;
    constexpr float y2 = 0.65f;
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

    add_static_box(dim, app->dimension_id, cube_id, "floor", {0.0f, -0.55f, 0.0f}, {40.0f, 1.0f, 40.0f}, {118, 134, 123, 255}, false, false);
    add_static_box(dim, app->dimension_id, cube_id, "north wall", {0.0f, 1.5f, -18.0f}, {40.0f, 4.0f, 1.0f}, {96, 105, 124, 255}, false, true);
    add_static_box(dim, app->dimension_id, cube_id, "west wall", {-18.0f, 1.5f, 0.0f}, {1.0f, 4.0f, 40.0f}, {93, 113, 123, 255}, false, true);
    add_static_box(dim, app->dimension_id, cube_id, "block a", {4.0f, 0.5f, -4.0f}, {2.5f, 2.0f, 2.5f}, {174, 96, 88, 255});
    add_static_box(dim, app->dimension_id, cube_id, "block b", {-4.0f, 0.25f, -2.0f}, {2.0f, 1.5f, 5.0f}, {96, 145, 112, 255});
    add_static_box(dim, app->dimension_id, cube_id, "tunnel left", {9.0f, 0.5f, -3.5f}, {0.45f, 1.0f, 5.0f}, {108, 120, 145, 255}, false, false);
    add_static_box(dim, app->dimension_id, cube_id, "tunnel right", {12.0f, 0.5f, -3.5f}, {0.45f, 1.0f, 5.0f}, {108, 120, 145, 255}, false, false);
    add_static_box(dim, app->dimension_id, cube_id, "tunnel roof", {10.5f, 1.15f, -3.5f}, {3.45f, 0.30f, 5.0f}, {93, 102, 130, 255}, false, false);
    add_contour_mesh(dim, app->dimension_id, make_tunnel_contour_geometry("tunnel contours"), "tunnel contours", {10.5f, 0.65f, -3.5f}, 3.2f);

    add_static_box(dim, app->dimension_id, cube_id, "step 1", {-8.0f, 0.0f, 5.0f}, {3.0f, 1.0f, 3.0f}, {166, 151, 92, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "step 2", {-8.0f, 0.5f, 8.0f}, {3.0f, 2.0f, 3.0f}, {174, 160, 101, 255}, true, false);
    add_static_box(dim, app->dimension_id, cube_id, "step 3", {-8.0f, 1.0f, 11.0f}, {3.0f, 3.0f, 3.0f}, {185, 171, 112, 255}, true, false);
    add_contour_mesh(dim, app->dimension_id, make_stair_contour_geometry("stair contours"), "stair contours", {-8.0f, 0.0f, 8.0f}, 5.5f);

    MeshInstance ramp{};
    std::snprintf(ramp.name, sizeof(ramp.name), "visual ramp");
    ramp.geometry = wedge_id;
    ramp.origin = demo_pos(app->dimension_id, {7.0f, 0.5f, 6.0f}, dim->chunk_size_m);
    ramp.se3 = MatrixScale(5.0f, 1.0f, 5.0f);
    ramp.color = {117, 136, 182, 255};
    dimension_add_mesh(dim, ramp);

    BoxCollider ramp_collider{};
    ramp_collider.pos = demo_pos(app->dimension_id, {7.0f, 0.363f, 6.024f}, dim->chunk_size_m);
    ramp_collider.half = {2.5f, 0.12f, 2.55f};
    ramp_collider.axis_aligned = false;
    ramp_collider.rotation = QuaternionFromAxisAngle({1.0f, 0.0f, 0.0f}, -std::atan2(1.05f, 5.0f));
    ramp_collider.color = ramp.color;
    physics_add_box(&dim->physics, ramp_collider);

    add_static_box(dim, app->dimension_id, cube_id, "ramp landing", {7.0f, 0.5f, 10.6f}, {5.0f, 1.0f, 4.0f}, {107, 126, 168, 255}, false, true);

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

    LightSource light{};
    std::snprintf(light.name, sizeof(light.name), "warm light");
    light.pos = demo_pos(app->dimension_id, {0.0f, 5.5f, -5.0f}, dim->chunk_size_m);
    light.color = {255, 230, 180, 255};
    light.radius = 14.0f;
    light.intensity = 1.2f;
    dimension_add_light(dim, light);

    LightSource blue = light;
    std::snprintf(blue.name, sizeof(blue.name), "blue light");
    blue.pos = demo_pos(app->dimension_id, {-8.0f, 4.0f, 8.0f}, dim->chunk_size_m);
    blue.color = {120, 160, 255, 255};
    blue.radius = 10.0f;
    blue.intensity = 0.9f;
    dimension_add_light(dim, blue);

    add_local_player_to_generated_world(app, dim);
}

static void generate_hills_world(DemoApp* app) {
    world_init(&app->world);
    reset_remote_players(app);
    clear_streamed_chunk_records(app);
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

    LightSource sun{};
    std::snprintf(sun.name, sizeof(sun.name), "wide hill light");
    sun.pos = demo_pos(app->dimension_id, {-18.0f, 18.0f, 12.0f}, dim->chunk_size_m);
    sun.color = {255, 238, 202, 255};
    sun.radius = 46.0f;
    sun.intensity = 1.05f;
    dimension_add_light(dim, sun);

    add_local_player_to_generated_world(app, dim);
}

void demo_generate_world(DemoApp* app) {
    char normalized[32]{};
    copy_world_name(normalized, sizeof(normalized), app->world_name);
    copy_text(app->world_name, sizeof(app->world_name), normalized);
    if (is_landscape_world(app->world_name)) {
        generate_hills_world(app);
    } else {
        generate_playground_world(app);
    }
}

void demo_init(DemoApp* app) {
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
    InitWindow(1280, 720, "OL");
    SetExitKey(KEY_NULL);
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetTargetFPS(120);
    renderer_init(&app->renderer);
    net_init(&app->net);
    demo_generate_world(app);
}

void demo_shutdown(DemoApp* app) {
    record_current_world_state(app);
    save_profile(app);
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
    if (!app->mouse_captured && app->frame_input.mouse_left_pressed) {
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

static float raycast_world(DemoApp* app, Dimension* dim, const PlayerEntity* local, WorldPos ray_start, Vector3 ray_dir, float max_dist) {
    if (!dim) return max_dist;

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
            }
        }
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
        }
    }

    return best_t;
}

static void update_player_aim_ray(DemoApp* app, Dimension* dim, PlayerEntity* player) {
    if (!app || !dim || !player) return;

    player->aim_ray_active = app->mouse_captured && app->frame_input.mouse_left_down;
    if (!player->aim_ray_active) return;

    const CameraView view = make_player_camera_view(app, dim, player);
    const WorldPos ray_start = worldpos_offset(view.anchor, {0.0f, view.eye_height, 0.0f}, dim->chunk_size_m);
    const Vector3 ray_dir = forward_from_angles(view.yaw, view.pitch);
    constexpr float max_ray_dist = 64.0f;
    const float hit_t = raycast_world(app, dim, player, ray_start, ray_dir, max_ray_dist);

    player->aim_ray_start = ray_start;
    player->aim_ray_end = worldpos_offset(ray_start, ray_dir * hit_t, dim->chunk_size_m);
    canonicalize(&player->aim_ray_end, dim->chunk_size_m);
}

static void fixed_update(DemoApp* app, Dimension* dim, float dt) {
    PlayerEntity* player = local_player(app, dim);
    if (!player) return;
    update_landscape_streaming(app, dim, player);
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

    SavedSessionState* session = upsert_session(&app->profile, app->session_name, app->world_name);
    if (!session) return;

    NetPlayerState local = make_net_player_state(app, dim, player);
    local.peer_id = local_player_peer_id(app);
    upsert_player_state(session, saved_state_from_net(local));

    for (u32 slot = 0; slot < max_players; ++slot) {
        const u64 peer_id = app->remote_peer_ids[slot];
        const u32 player_id = app->remote_player_ids[slot];
        if (!peer_id || !id_valid(player_id)) continue;
        const PlayerEntity* remote = arena_get(&dim->players, player_id);
        if (!remote) continue;
        NetPlayerState remote_state = make_net_player_state(app, dim, remote);
        remote_state.peer_id = peer_id;
        upsert_player_state(session, saved_state_from_net(remote_state));
    }

    mark_profile_dirty(app);
}

static void autosave_profile_if_needed(DemoApp* app) {
    if (!app->profile_dirty) return;
    if (GetTime() - app->last_profile_save_time < 2.0) return;
    save_profile(app);
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
    if (!saved) return;

    net_send_player_restore(&app->net, peer_id, net_state_from_saved(*saved, app->dimension_id));
    remember_restore_sent(app, peer_id);
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
    net_leave(&app->net);
    reset_remote_players(app);
    app->in_game = false;
    app->paused = false;
    app->mouse_captured = false;
    app->blocking_screen_cursor_released = false;
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
        renderer_draw_dimension(&app->renderer, dim, view, app->local_player_id);
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
        dim ? dim->render_radius_chunks : 6
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
    if (!(app->net.mode == net_client && app->net.pending)) return false;

    release_cursor_for_blocking_screen(app);

    net_update(&app->net);
    if (app->net.in_lobby) {
        sync_session_from_network(app, dim, player);
        app->mouse_captured = true;
        app->blocking_screen_cursor_released = false;
        DisableCursor();
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
        if (hit.control == pause_control_fov || hit.control == pause_control_scale || hit.control == pause_control_render_radius) {
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
    update_landscape_streaming(app, dim, player);

    if (!app->paused && app->frame_input.escape_pressed) {
        pause_game(app);
        if (only_local_player(app)) {
            app->fixed_accum = 0.0f;
        } else {
            tick_game_simulation(app, dim, GetFrameTime());
        }
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
    if (app->frame_input.r_pressed) {
        respawn_player_at_default(app, dim, player);
        app->fixed_accum = 0.0f;
        app->input = {};
    }
    if (app->frame_input.c_pressed && app->net.in_lobby) net_copy_lobby_to_clipboard(&app->net);

    collect_input(app, player);
    tick_game_simulation(app, dim, GetFrameTime());
    update_landscape_streaming(app, dim, player);
    update_player_aim_ray(app, dim, player);
    if (update_network_state(app, dim, player)) {
        dim = world_get_dimension(&app->world, app->dimension_id);
        player = local_player(app, dim);
        if (!dim || !player) return;
    }
    if (update_host_left_screen(app)) return;

    draw_game_scene(app, dim, player, true, true);
}

bool demo_update_and_draw(DemoApp* app) {
    if (WindowShouldClose()) return false;
    capture_frame_input(app);
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
    SetTraceLogLevel(LOG_WARNING);
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
    SetTraceLogLevel(LOG_WARNING);
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
