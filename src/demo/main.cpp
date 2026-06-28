#include "demo/demo.h"

#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--steam-host-smoke") == 0) {
        const double timeout_s = argc >= 3 ? std::atof(argv[2]) : 20.0;
        const double hold_s = argc >= 4 ? std::atof(argv[3]) : 0.0;
        return ol::demo_run_steam_host_smoke(timeout_s > 0.0 ? timeout_s : 20.0, hold_s > 0.0 ? hold_s : 0.0);
    }
    if (argc >= 3 && std::strcmp(argv[1], "--steam-join-smoke") == 0) {
        const double timeout_s = argc >= 4 ? std::atof(argv[3]) : 20.0;
        return ol::demo_run_steam_join_smoke(argv[2], timeout_s > 0.0 ? timeout_s : 20.0);
    }

    auto* app = new ol::DemoApp();
    ol::demo_init(app);
    while (ol::demo_update_and_draw(app)) {
    }
    ol::demo_shutdown(app);
    delete app;
    return 0;
}
