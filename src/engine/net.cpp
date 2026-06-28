#include "engine/net.h"

#include <cstdlib>
#include <cstdio>

#ifdef OL_USE_STEAMWORKS
#include "steam/steam_api.h"
#include "steam/isteamnetworkingmessages.h"
#endif

namespace ol {

static void net_status(NetSession* net, const char* fmt, const char* arg = nullptr) {
    if (arg) {
        std::snprintf(net->status, sizeof(net->status), fmt, arg);
    } else {
        std::snprintf(net->status, sizeof(net->status), "%s", fmt);
    }
}

void net_init(NetSession* net) {
    *net = NetSession{};
#ifdef OL_USE_STEAMWORKS
    net->steam_available = SteamAPI_Init();
    net_status(net, net->steam_available ? "Steam ready" : "Steam init failed; offline mode");
#else
    net->steam_available = false;
    net_status(net, "Offline build; Steamworks disabled");
#endif
}

void net_shutdown(NetSession* net) {
#ifdef OL_USE_STEAMWORKS
    if (net->steam_available) SteamAPI_Shutdown();
#else
    (void)net;
#endif
}

void net_host(NetSession* net, const char* session_name) {
    net->mode = net_hosting;
    net->lobby_owner = true;
    net->in_lobby = false;
    net->pending = false;
#ifdef OL_USE_STEAMWORKS
    if (net->steam_available && SteamMatchmaking()) {
        SteamMatchmaking()->CreateLobby(k_ELobbyTypeFriendsOnly, max_players);
        net->pending = true;
        net_status(net, "Creating Steam lobby");
        return;
    }
#endif
    net_status(net, "Hosting offline session: %s", session_name ? session_name : "session");
}

void net_join_from_clipboard(NetSession* net) {
#ifdef OL_USE_STEAMWORKS
    const char* clip = GetClipboardText();
    if (!net->steam_available || !SteamMatchmaking() || !clip || !clip[0]) {
        net_status(net, "No lobby id in clipboard or Steam unavailable");
        return;
    }
    const u64 id = static_cast<u64>(std::strtoull(clip, nullptr, 10));
    if (id == 0) {
        net_status(net, "Clipboard does not contain a lobby id");
        return;
    }
    SteamMatchmaking()->JoinLobby(CSteamID(id));
    net->mode = net_client;
    net->pending = true;
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

void net_update(NetSession* net) {
#ifdef OL_USE_STEAMWORKS
    if (net->steam_available) {
        SteamAPI_RunCallbacks();
        if (net->pending) net_status(net, "Steam lobby request pending");
    }
#else
    (void)net;
#endif
}

} // namespace ol
