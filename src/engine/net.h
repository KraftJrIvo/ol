#pragma once

#include "engine/base.h"
#include "engine/math.h"

#include "raylib.h"

#include <array>

namespace ol {

enum NetMode : u32 {
    net_offline,
    net_hosting,
    net_client
};

enum NetPacketType : u32 {
    net_packet_input = 1,
    net_packet_snapshot = 2,
    net_packet_join = 3,
    net_packet_welcome = 4
};

struct NetPlayerState {
    u32 player_id = invalid_id;
    u32 dimension_id = invalid_id;
    ChunkCoord chunk{};
    Vector3 local = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    Color color = WHITE;
    char name[32]{};
};

struct ClientInputPacket {
    NetPacketType type = net_packet_input;
    u32 tick = 0;
    u32 sequence = 0;
    u32 buttons = 0;
    float move_x = 0.0f;
    float move_y = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct ServerSnapshotPacket {
    NetPacketType type = net_packet_snapshot;
    u32 tick = 0;
    u32 player_count = 0;
    std::array<NetPlayerState, max_players> players{};
};

struct NetSession {
    NetMode mode = net_offline;
    bool steam_available = false;
    bool in_lobby = false;
    bool lobby_owner = false;
    bool pending = false;
    u64 lobby_id = 0;
    u32 local_tick = 0;
    u32 last_sent_sequence = 0;
    double last_send_time = 0.0;
    char status[128]{};
};

void net_init(NetSession* net);
void net_shutdown(NetSession* net);
void net_host(NetSession* net, const char* session_name);
void net_join_from_clipboard(NetSession* net);
void net_copy_lobby_to_clipboard(NetSession* net);
void net_update(NetSession* net);

} // namespace ol
