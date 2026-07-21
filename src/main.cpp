#include "engine/application.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>

int main(int argc, char** argv) {
    try {
        // SPIR-V modules are staged next to the executable by the build.
        std::filesystem::path shaderDirectory = "shaders";
        if (argc > 0 && argv[0] != nullptr) {
            shaderDirectory = std::filesystem::absolute(argv[0]).parent_path() / "shaders";
        }

        // --frames N runs a fixed number of frames and then shuts down
        // normally, so the whole lifecycle can be exercised unattended.
        uint64_t maxFrames = 0;
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--frames") == 0) {
                maxFrames = std::strtoull(argv[i + 1], nullptr, 10);
            }
        }

        engine::AppConfig config{
            .width  = 1280,
            .height = 720,
            .title  = "fitzel",
            // The only place in engine-level code that names a backend.
            .backend = rhi::Backend::Vulkan,
#ifdef NDEBUG
            .enableValidation = false,
#else
            .enableValidation = true,
#endif
            .shaderDirectory = shaderDirectory,
            .maxFrames       = maxFrames,
        };

        engine::Application app(config);
        app.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
