include(CompileShaders)

fitzel_add_shaders(fitzel_shaders
    TARGET_ENV vulkan1.3
    SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/raymarch_probe.comp")
