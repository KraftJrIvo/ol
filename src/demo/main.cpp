#include "demo/demo.h"

int main() {
    auto* app = new ol::DemoApp();
    ol::demo_init(app);
    while (ol::demo_update_and_draw(app)) {
    }
    ol::demo_shutdown(app);
    delete app;
    return 0;
}
