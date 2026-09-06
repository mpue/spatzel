set(FITZEL_SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/raymarch.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/raymarch_brick.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bake_classify.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bake_fill.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/pick.comp")

# Shared #include files: the scene evaluation, the shading model and the brick
# structure. They are not compiled on their own — they carry no entry point —
# but changing one must rebuild every module that pulls it in.
set(FITZEL_SHADER_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/sdf_scene.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shading.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/brick_common.glsl")

if(FITZEL_WITH_VULKAN)
    fitzel_add_shaders(fitzel_shaders_vulkan
        SUBDIR     vulkan
        TARGET_ENV vulkan1.3
        DEFINES    TARGET_VULKAN
        INCLUDES   ${FITZEL_SHADER_INCLUDES}
        SOURCES    ${FITZEL_SHADER_SOURCES})
endif()

if(FITZEL_WITH_OPENGL)
    fitzel_add_shaders(fitzel_shaders_opengl
        SUBDIR     opengl
        TARGET_ENV opengl
        DEFINES    TARGET_OPENGL
        INCLUDES   ${FITZEL_SHADER_INCLUDES}
        SOURCES    ${FITZEL_SHADER_SOURCES})
endif()
