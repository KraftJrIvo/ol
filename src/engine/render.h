#pragma once

#include "engine/world.h"

#include "raylib.h"

namespace ol {

struct CameraView {
    WorldPos anchor{};
    float eye_height = 1.4f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct RenderState {
    RenderTexture2D target{};
    Texture2D white_texture{};
    Font font{};
    bool target_ready = false;
    bool white_ready = false;
    bool font_ready = false;
    int native_w = 0;
    int native_h = 0;
    int window_w = 0;
    int window_h = 0;
    int scale_power = 0;
    float fov = 90.0f;
    bool draw_physics_debug = false;
};

void renderer_init(RenderState* renderer);
void renderer_shutdown(RenderState* renderer);
void renderer_change_scale(RenderState* renderer, int delta);
void renderer_ensure_target(RenderState* renderer);
void renderer_draw_dimension(RenderState* renderer, Dimension* dim, const CameraView& view, u32 local_player_id);

} // namespace ol
