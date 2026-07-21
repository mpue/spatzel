#include "platform/window.hpp"

#include <cstdio>
#include <exception>

int main() {
    try {
        platform::Window window({.width = 1280, .height = 720, .title = "fitzel"});

        while (!window.shouldClose()) {
            window.pollEvents();
            if (window.isKeyDown(platform::Key::Escape)) {
                window.requestClose();
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
