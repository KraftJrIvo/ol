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
    net_packet_welcome = 4,
    net_packet_player_state = 5
};

struct NetPlayerState {
    u64 peer_id = 0;
    u32 player_id = invalid_id;
    u32 dimension_id = invalid_id;
    ChunkCoord chunk{};
    Vector3 local = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float body_radius = 0.35f;
    float current_height = 1.80f;
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

struct NetPlayerStatePacket {
    NetPacketType type = net_packet_player_state;
    NetPlayerState player{};
};

struct NetSession {
    NetMode mode = net_offline;
    bool steam_available = false;
    bool in_lobby = false;
    bool lobby_owner = false;
    bool pending = false;
    u64 lobby_id = 0;
    u64 local_peer_id = 0;
    u32 local_tick = 0;
    u32 last_sent_sequence = 0;
    u32 peer_count = 0;
    double last_send_time = 0.0;
    std::array<u64, max_players> peer_ids{};
    std::array<NetPlayerState, max_players> remote_players{};
    std::array<bool, max_players> remote_player_valid{};
    NetPlayerState local_player{};
    bool local_player_valid = false;
    char session_name[32] = "session";
    char status[128]{};
    void* steam_state = nullptr;
};

void net_init(NetSession* net);
void net_shutdown(NetSession* net);
void net_host(NetSession* net, const char* session_name);
void net_join_from_clipboard(NetSession* net);
void net_copy_lobby_to_clipboard(NetSession* net);
void net_set_local_player(NetSession* net, const NetPlayerState& player);
void net_update(NetSession* net);

} // namespace ol
