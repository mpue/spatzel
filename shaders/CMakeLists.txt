set(FITZEL_SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/raymarch_probe.comp")

if(FITZEL_WITH_VULKAN)
    fitzel_add_shaders(fitzel_shaders_vulkan
        SUBDIR     vulkan
        TARGET_ENV vulkan1.3
        DEFINES    TARGET_VULKAN
        SOURCES    ${FITZEL_SHADER_SOURCES})
endif()

if(FITZEL_WITH_OPENGL)
    fitzel_add_shaders(fitzel_shaders_opengl
        SUBDIR     opengl
        TARGET_ENV opengl
        DEFINES    TARGET_OPENGL
        SOURCES    ${FITZEL_SHADER_SOURCES})
endif()
