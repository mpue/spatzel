#include "engine/application.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "usage: fitzel [options]\n"
                 "  --backend <vulkan|opengl>  graphics backend (default: vulkan)\n"
                 "  --frames <n>               run n frames, then shut down normally\n"
                 "  --dump <file>              write the final frame for later comparison\n"
                 "  --compare <file>           compare the final frame against a dump\n"
                 "  --tolerance <f>            per-component tolerance for --compare\n");
}

bool parseBackend(std::string_view name, rhi::Backend& out) {
    // The only place outside the RHI where a backend name is spelled out.
    if (name == "vulkan") {
        out = rhi::Backend::Vulkan;
        return true;
    }
    if (name == "opengl") {
        out = rhi::Backend::OpenGL;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    try {
        engine::AppConfig config{};
        config.title = "fitzel";
#ifndef NDEBUG
        config.enableDebug = true;
#endif

        // SPIR-V variants are staged next to the executable by the build.
        if (argc > 0 && argv[0] != nullptr) {
            config.shaderRoot = std::filesystem::absolute(argv[0]).parent_path() / "shaders";
        }

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            const bool             hasValue = i + 1 < argc;

            if (arg == "--backend" && hasValue) {
                if (!parseBackend(argv[++i], config.backend)) {
                    std::fprintf(stderr, "unknown backend '%s'\n", argv[i]);
                    printUsage();
                    return 2;
                }
            } else if (arg == "--frames" && hasValue) {
                config.maxFrames = std::strtoull(argv[++i], nullptr, 10);
            } else if (arg == "--dump" && hasValue) {
                config.dumpPath = argv[++i];
            } else if (arg == "--compare" && hasValue) {
                config.comparePath = argv[++i];
            } else if (arg == "--tolerance" && hasValue) {
                config.tolerance = std::strtof(argv[++i], nullptr);
            } else {
                std::fprintf(stderr, "unrecognised argument '%s'\n", argv[i]);
                printUsage();
                return 2;
            }
        }

        if (!rhi::isBackendAvailable(config.backend)) {
            std::fprintf(stderr, "requested backend is not linked into this binary\n");
            return 2;
        }

        engine::Application app(config);
        return app.run() ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
