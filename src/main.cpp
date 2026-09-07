#include "engine/application.hpp"
#include "engine/scene_io.hpp"

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
                 "  --renderer <brick|reference>  renderer (default: brick)\n"
                 "  --debug-view <0|1|2>       brick view: 0 shaded, 1 step heat, 2 brick tint\n"
                 "  --frames <n>               run n frames, then shut down normally\n"
                 "  --dump <file>              write the final frame for later comparison\n"
                 "  --compare <file>           compare the final frame against a dump\n"
                 "  --tolerance <f>            per-component tolerance for --compare\n"
                 "  --max-outlier-fraction <f> pass --compare if at most this fraction of\n"
                 "                             components exceed tolerance (default 0 = strict max)\n"
                 "  --no-ui                    disable the editor overlay\n"
                 "  --fluid                    start with the water simulation enabled\n"
                 "  --play                     start the animation clip playing (looping)\n"
                 "  --scene <file>             load this scene JSON at startup\n"
                 "  --save-scene <file>        write the built-in (or --scene) list and exit\n"
                 "\n"
                 "runtime keys: 1 reference, 2 brick, 3 cycle brick debug view\n");
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

bool parseRenderer(std::string_view name, engine::RendererMode& out) {
    if (name == "brick") {
        out = engine::RendererMode::Brick;
        return true;
    }
    if (name == "reference") {
        out = engine::RendererMode::Reference;
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
        std::filesystem::path saveScenePath;

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
            } else if (arg == "--renderer" && hasValue) {
                if (!parseRenderer(argv[++i], config.renderer)) {
                    std::fprintf(stderr, "unknown renderer '%s'\n", argv[i]);
                    printUsage();
                    return 2;
                }
            } else if (arg == "--debug-view" && hasValue) {
                config.debugView = static_cast<int32_t>(std::strtol(argv[++i], nullptr, 10));
            } else if (arg == "--frames" && hasValue) {
                config.maxFrames = std::strtoull(argv[++i], nullptr, 10);
            } else if (arg == "--dump" && hasValue) {
                config.dumpPath = argv[++i];
            } else if (arg == "--compare" && hasValue) {
                config.comparePath = argv[++i];
            } else if (arg == "--tolerance" && hasValue) {
                config.tolerance = std::strtof(argv[++i], nullptr);
            } else if (arg == "--max-outlier-fraction" && hasValue) {
                config.maxOutlierFraction = std::strtof(argv[++i], nullptr);
            } else if (arg == "--fluid") {
                config.fluid = true;
            } else if (arg == "--play") {
                config.play = true;
            } else if (arg == "--no-ui") {
                config.enableUi = false;
            } else if (arg == "--scene" && hasValue) {
                config.scenePath = argv[++i];
            } else if (arg == "--save-scene" && hasValue) {
                saveScenePath = argv[++i];
            } else {
                std::fprintf(stderr, "unrecognised argument '%s'\n", argv[i]);
                printUsage();
                return 2;
            }
        }

        // Authoring helper: write the built-in (or --scene) list to a file and
        // exit, without opening a window. Used to seed the example scenes from
        // the canonical buildScene().
        if (!saveScenePath.empty()) {
            engine::fluid::Settings fluidSettings{};
            const std::vector<engine::GpuPrimitive> scene =
                config.scenePath.empty()
                    ? engine::buildScene()
                    : engine::loadScene(config.scenePath, nullptr, &fluidSettings);
            engine::saveScene(saveScenePath, scene, nullptr, &fluidSettings);
            std::fprintf(stderr, "[scene] wrote %s\n", saveScenePath.string().c_str());
            return 0;
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
