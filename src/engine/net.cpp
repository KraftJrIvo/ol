#include "engine/net.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef OL_USE_STEAMWORKS
#include "steam/steam_api.h"
#include "steam/isteamnetworkingmessages.h"
#include "steam/isteamnetworkingutils.h"
#endif

namespace ol {

static void net_status(NetSession* net, const char* fmt, ...) {
    if (!net || !fmt) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(net->status, sizeof(net->status), fmt, args);
    va_end(args);
}

static void net_clear_peers(NetSession* net) {
    net->peer_count = 0;
    net->peer_ids.fill(0);
    net->remote_players.fill(NetPlayerState{});
    net->remote_player_valid.fill(false);
}

static bool net_find_peer(const NetSession* net, u64 peer_id, u32* out_idx) {
    for (u32 i = 0; i < net->peer_count; ++i) {
        if (net->peer_ids[i] == peer_id) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

static u32 net_add_peer(NetSession* net, u64 peer_id) {
    if (!peer_id || peer_id == net->local_peer_id) return invalid_id;

    u32 existing = invalid_id;
    if (net_find_peer(net, peer_id, &existing)) return existing;
    if (net->peer_count >= max_players - 1) return invalid_id;

    const u32 idx = net->peer_count++;
    net->peer_ids[idx] = peer_id;
    net->remote_players[idx] = NetPlayerState{};
    net->remote_players[idx].peer_id = peer_id;
    net->remote_player_valid[idx] = false;
    return idx;
}

static void net_remove_peer_at(NetSession* net, u32 idx) {
    if (idx >= net->peer_count) return;
    const u32 last = net->peer_count - 1;
    if (idx != last) {
        net->peer_ids[idx] = net->peer_ids[last];
        net->remote_players[idx] = net->remote_players[last];
        net->remote_player_valid[idx] = net->remote_player_valid[last];
    }
    net->peer_ids[last] = 0;
    net->remote_players[last] = NetPlayerState{};
    net->remote_player_valid[last] = false;
    net->peer_count--;
}

static bool parse_lobby_id_text(const char* text, u64* out_id) {
    if (!text || !out_id) return false;

    char digits[32]{};
    u32 count = 0;
    for (const char* at = text; *at && count + 1 < sizeof(digits); ++at) {
        if (*at >= '0' && *at <= '9') {
            digits[count++] = *at;
        }
    }
    if (count == 0) return false;

    char* end = nullptr;
    const unsigned long long value = std::strtoull(digits, &end, 10);
    if (!end || *end != 0 || value == 0) return false;

    *out_id = static_cast<u64>(value);
    return true;
}

#ifdef OL_USE_STEAMWORKS

struct SteamNetCallbacks {
    NetSession* net = nullptr;
    void OnLobbyCreated(LobbyCreated_t* res, bool io_failure);
    void OnLobbyEnter(LobbyEnter_t* res, bool io_failure);
};

struct SteamNetState {
    SteamNetCallbacks callbacks{};
    CCallResult<SteamNetCallbacks, LobbyCreated_t> lobby_created{};
    CCallResult<SteamNetCallbacks, LobbyEnter_t> lobby_enter{};
    bool lobby_metadata_published = false;
    bool member_metadata_published = false;
};

static SteamNetState* steam_state(NetSession* net) {
    return static_cast<SteamNetState*>(net->steam_state);
}

static CSteamID steam_lobby_id(const NetSession* net) {
    return CSteamID(static_cast<uint64>(net->lobby_id));
}

static void steam_copy_lobby_id_to_clipboard(NetSession* net) {
    if (!net->lobby_id) return;
    char text[32]{};
    std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(net->lobby_id));
    SetClipboardText(text);
}

static void steam_publish_lobby_metadata(NetSession* net) {
    if (!net->in_lobby || !SteamMatchmaking()) return;
    SteamNetState* state = steam_state(net);
    if (!state) return;

    const CSteamID lobby = steam_lobby_id(net);
    if (net->lobby_owner && !state->lobby_metadata_published) {
        SteamMatchmaking()->SetLobbyJoinable(lobby, true);
        SteamMatchmaking()->SetLobbyData(lobby, "name", net->session_name);
        SteamMatchmaking()->SetLobbyData(lobby, "ol_version", "0.1");
        state->lobby_metadata_published = true;
    }

    if (net->local_player_valid && !state->member_metadata_published) {
        SteamMatchmaking()->SetLobbyMemberData(lobby, "name", net->local_player.name);
        char color[16]{};
        std::snprintf(color, sizeof(color), "%u,%u,%u",
            static_cast<unsigned>(net->local_player.color.r),
            static_cast<unsigned>(net->local_player.color.g),
            static_cast<unsigned>(net->local_player.color.b));
        SteamMatchmaking()->SetLobbyMemberData(lobby, "color", color);
        state->member_metadata_published = true;
    }
}

static void steam_finish_lobby_enter(NetSession* net, CSteamID lobby, bool created_by_local_user) {
    net->pending = false;
    net->in_lobby = true;
    net->lobby_id = lobby.ConvertToUint64();
    net->local_peer_id = SteamUser() ? SteamUser()->GetSteamID().ConvertToUint64() : 0;

    CSteamID owner = SteamMatchmaking() ? SteamMatchmaking()->GetLobbyOwner(lobby) : CSteamID();
    const CSteamID local = SteamUser() ? SteamUser()->GetSteamID() : CSteamID();
    net->lobby_owner = created_by_local_user || (owner.IsValid() && local.IsValid() && owner == local);
    net->mode = net->lobby_owner ? net_hosting : net_client;

    steam_publish_lobby_metadata(net);
    if (net->lobby_owner) {
        steam_copy_lobby_id_to_clipboard(net);
        net_status(net, "Hosting Steam lobby %llu; id copied to clipboard",
            static_cast<unsigned long long>(net->lobby_id));
    } else {
        net_status(net, "Joined Steam lobby %llu",
            static_cast<unsigned long long>(net->lobby_id));
    }
}

void SteamNetCallbacks::OnLobbyCreated(LobbyCreated_t* res, bool io_failure) {
    if (!net) return;
    net->pending = false;
    if (io_failure || !res || res->m_eResult != k_EResultOK) {
        net->mode = net_offline;
        net->in_lobby = false;
        net->lobby_owner = false;
        net_status(net, "Steam lobby creation failed");
        return;
    }

    steam_finish_lobby_enter(net, CSteamID(res->m_ulSteamIDLobby), true);
}

void SteamNetCallbacks::OnLobbyEnter(LobbyEnter_t* res, bool io_failure) {
    if (!net) return;
    net->pending = false;
    if (io_failure || !res || res->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
        net->mode = net_offline;
        net->in_lobby = false;
        net->lobby_owner = false;
        net_status(net, "Steam lobby join failed");
        return;
    }

    steam_finish_lobby_enter(net, CSteamID(res->m_ulSteamIDLobby), false);
}

static void steam_on_messages_session_request(SteamNetworkingMessagesSessionRequest_t* request) {
    if (!request || !SteamNetworkingMessages()) return;
    SteamNetworkingMessages()->AcceptSessionWithUser(request->m_identityRemote);
}

static void steam_sync_lobby_members(NetSession* net) {
    if (!net->in_lobby || !SteamMatchmaking() || !SteamUser()) return;

    const CSteamID lobby = steam_lobby_id(net);
    const CSteamID local = SteamUser()->GetSteamID();
    bool changed = false;

    const int count = SteamMatchmaking()->GetNumLobbyMembers(lobby);
    for (int i = 0; i < count; ++i) {
        const CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(lobby, i);
        if (!member.IsValid() || member == local) continue;
        const u32 before = net->peer_count;
        net_add_peer(net, member.ConvertToUint64());
        changed = changed || before != net->peer_count;
    }

    for (u32 i = 0; i < net->peer_count;) {
        bool still_in_lobby = false;
        for (int member_idx = 0; member_idx < count; ++member_idx) {
            const CSteamID member = SteamMatchmaking()->GetLobbyMemberByIndex(lobby, member_idx);
            if (member.IsValid() && member.ConvertToUint64() == net->peer_ids[i]) {
                still_in_lobby = true;
                break;
            }
        }

        if (!still_in_lobby) {
            SteamNetworkingIdentity peer_identity{};
            peer_identity.SetSteamID64(net->peer_ids[i]);
            if (SteamNetworkingMessages()) {
                SteamNetworkingMessages()->CloseSessionWithUser(peer_identity);
            }
            net_remove_peer_at(net, i);
            changed = true;
        } else {
            ++i;
        }
    }

    const CSteamID owner = SteamMatchmaking()->GetLobbyOwner(lobby);
    const bool owner_now = owner.IsValid() && owner == local;
    if (owner_now != net->lobby_owner) {
        net->lobby_owner = owner_now;
        net->mode = owner_now ? net_hosting : net_client;
        changed = true;
    }

    if (changed) {
        net_status(net, "Steam lobby %llu | peers %u",
            static_cast<unsigned long long>(net->lobby_id),
            static_cast<unsigned>(net->peer_count));
    }
}

static void steam_receive(NetSession* net) {
    if (!net->in_lobby || !SteamNetworkingMessages()) return;

    SteamNetworkingMessage_t* incoming[16]{};
    const int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(0, incoming, 16);
    for (int i = 0; i < count; ++i) {
        SteamNetworkingMessage_t* msg = incoming[i];
        if (!msg) continue;

        const u64 from = msg->m_identityPeer.GetSteamID64();
        if (from && from != net->local_peer_id && msg->m_cbSize == static_cast<int>(sizeof(NetPlayerStatePacket))) {
            NetPlayerStatePacket packet{};
            std::memcpy(&packet, msg->m_pData, sizeof(packet));
            if (packet.type == net_packet_player_state) {
                const u32 peer_idx = net_add_peer(net, from);
                if (id_valid(peer_idx)) {
                    packet.player.peer_id = from;
                    net->remote_players[peer_idx] = packet.player;
                    net->remote_player_valid[peer_idx] = true;
                }
            }
        }

        msg->Release();
    }
}

static void steam_send(NetSession* net) {
    if (!net->in_lobby || !net->local_player_valid || !SteamNetworkingMessages()) return;
    if (net->peer_count == 0) return;

    const double now = GetTime();
    if (now - net->last_send_time < (1.0 / 30.0)) return;
    net->last_send_time = now;

    NetPlayerStatePacket packet{};
    packet.player = net->local_player;
    packet.player.peer_id = net->local_peer_id;

    for (u32 i = 0; i < net->peer_count; ++i) {
        SteamNetworkingIdentity peer_identity{};
        peer_identity.SetSteamID64(net->peer_ids[i]);
        SteamNetworkingMessages()->SendMessageToUser(
            peer_identity,
            &packet,
            sizeof(packet),
            k_nSteamNetworkingSend_UnreliableNoDelay | k_nSteamNetworkingSend_AutoRestartBrokenSession,
            0
        );
    }
}

#endif

void net_init(NetSession* net) {
    *net = NetSession{};
    net_clear_peers(net);
#ifdef OL_USE_STEAMWORKS
    net->steam_available = SteamAPI_Init();
    if (net->steam_available) {
        auto* state = new SteamNetState();
        state->callbacks.net = net;
        net->steam_state = state;
        net->local_peer_id = SteamUser() ? SteamUser()->GetSteamID().ConvertToUint64() : 0;
        if (SteamNetworkingUtils()) {
            SteamNetworkingUtils()->SetGlobalCallback_MessagesSessionRequest(steam_on_messages_session_request);
        }
    }
    net_status(net, net->steam_available ? "Steam ready" : "Steam init failed; offline mode");
#else
    net->steam_available = false;
    net_status(net, "Offline build; Steamworks disabled");
#endif
}

void net_shutdown(NetSession* net) {
#ifdef OL_USE_STEAMWORKS
    if (net->steam_available) {
        if (net->in_lobby && SteamMatchmaking()) {
            SteamMatchmaking()->LeaveLobby(steam_lobby_id(net));
        }
        if (SteamNetworkingMessages()) {
            for (u32 i = 0; i < net->peer_count; ++i) {
                SteamNetworkingIdentity peer_identity{};
                peer_identity.SetSteamID64(net->peer_ids[i]);
                SteamNetworkingMessages()->CloseSessionWithUser(peer_identity);
            }
        }
        delete steam_state(net);
        net->steam_state = nullptr;
        SteamAPI_Shutdown();
    }
#else
    (void)net;
#endif
}

void net_host(NetSession* net, const char* session_name) {
    std::snprintf(net->session_name, sizeof(net->session_name), "%s",
        session_name && session_name[0] ? session_name : "session");
    net_clear_peers(net);
    net->mode = net_hosting;
    net->lobby_owner = true;
    net->in_lobby = false;
    net->pending = false;
    net->lobby_id = 0;
#ifdef OL_USE_STEAMWORKS
    if (net->steam_available && SteamMatchmaking() && steam_state(net)) {
        steam_state(net)->lobby_metadata_published = false;
        steam_state(net)->member_metadata_published = false;
        const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, max_players);
        if (call == k_uAPICallInvalid) {
            net_status(net, "Steam lobby creation failed to start");
            return;
        }
        steam_state(net)->lobby_created.Set(call, &steam_state(net)->callbacks, &SteamNetCallbacks::OnLobbyCreated);
        net->pending = true;
        net_status(net, "Creating Steam lobby");
        return;
    }
#endif
    net_status(net, "Hosting offline session: %s", net->session_name);
}

void net_join_from_clipboard(NetSession* net) {
#ifdef OL_USE_STEAMWORKS
    const char* clip = GetClipboardText();
    u64 lobby_id = 0;
    if (!net->steam_available || !SteamMatchmaking() || !steam_state(net) || !parse_lobby_id_text(clip, &lobby_id)) {
        net_status(net, "No lobby id in clipboard or Steam unavailable");
        return;
    }

    CSteamID lobby(lobby_id);
    if (!lobby.IsValid()) {
        net_status(net, "Clipboard does not contain a valid lobby id");
        return;
    }

    net_clear_peers(net);
    steam_state(net)->lobby_metadata_published = false;
    steam_state(net)->member_metadata_published = false;
    net->mode = net_client;
    net->lobby_owner = false;
    net->in_lobby = false;
    net->pending = true;
    net->lobby_id = lobby_id;

    const SteamAPICall_t call = SteamMatchmaking()->JoinLobby(lobby);
    if (call == k_uAPICallInvalid) {
        net->pending = false;
        net_status(net, "Steam lobby join failed to start");
        return;
    }

    steam_state(net)->lobby_enter.Set(call, &steam_state(net)->callbacks, &SteamNetCallbacks::OnLobbyEnter);
    net_status(net, "Joining Steam lobby");
#else
    (void)net;
    net_status(net, "Join requires OL_USE_STEAMWORKS");
#endif
}

void net_copy_lobby_to_clipboard(NetSession* net) {
    if (!net->lobby_id) {
        net_status(net, "No lobby id to copy");
        return;
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(net->lobby_id));
    SetClipboardText(text);
    net_status(net, "Lobby id copied");
}

void net_set_local_player(NetSession* net, const NetPlayerState& player) {
    net->local_player = player;
    net->local_player.peer_id = net->local_peer_id;
    net->local_player_valid = true;
}

void net_update(NetSession* net) {
#ifdef OL_USE_STEAMWORKS
    if (net->steam_available) {
        SteamAPI_RunCallbacks();
        if (net->in_lobby) {
            steam_sync_lobby_members(net);
            steam_publish_lobby_metadata(net);
            steam_receive(net);
            steam_send(net);
        }
    }
#else
    (void)net;
#endif
}

} // namespace ol
