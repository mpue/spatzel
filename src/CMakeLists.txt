# Target layout mirrors the architecture:
#
#   fitzel_platform    windowing/input/timing        (glfw + rhi.hpp, no backend)
#   fitzel_rhi         the seam, header-only         (no dependencies at all)
#   fitzel_rhi_vulkan  Vulkan-aware target           (volk, VMA, vk-bootstrap)
#   fitzel_rhi_opengl  GL-aware target               (glad)
#   fitzel_rhi_factory backend registry              (rhi.hpp + backend headers)
#   fitzel_engine      frame orchestration           (rhi + platform, NO backend)
#   fitzel             executable, links the registry
#
# fitzel_engine deliberately links no backend. The registry is the only place
# outside src/rhi/<backend>/ that names one, and it does so through headers
# that contain no graphics API symbols.

add_library(fitzel_rhi INTERFACE)
target_include_directories(fitzel_rhi INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# The platform layer sees rhi.hpp only for rhi::WindowRequirements: a
# client-API context must exist from window creation onwards and cannot be
# attached later. It also owns the ImGui GLFW backend — pure windowing glue,
# no graphics API — so the input side of the UI stays in the platform layer.
add_library(fitzel_platform STATIC
    platform/window.cpp)
target_include_directories(fitzel_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fitzel_platform PUBLIC glfw fitzel_rhi PRIVATE imgui_glfw fitzel_warnings)

add_library(fitzel_rhi_factory STATIC
    rhi/factory.cpp)
target_link_libraries(fitzel_rhi_factory PUBLIC fitzel_rhi PRIVATE fitzel_warnings)

if(FITZEL_WITH_VULKAN)
    add_library(fitzel_rhi_vulkan STATIC
        rhi/vulkan/vk_common.cpp
        rhi/vulkan/vk_swapchain.cpp
        rhi/vulkan/vk_device.cpp
        rhi/vulkan/vk_command_list.cpp
        rhi/vulkan/vk_factory.cpp
        # ImGui's Vulkan render backend. API-specific, so it lives with the
        # Vulkan backend and nowhere above it.
        "${FITZEL_IMGUI_BACKENDS_DIR}/imgui_impl_vulkan.cpp")
    target_link_libraries(fitzel_rhi_vulkan
        PUBLIC  fitzel_rhi
        PRIVATE volk
                GPUOpen::VulkanMemoryAllocator
                vk-bootstrap::vk-bootstrap
                glfw
                imgui
                fitzel_warnings)
    # volk owns every entry point, so VMA must resolve its own through the two
    # loader functions it is handed at allocator creation. ImGui's Vulkan
    # backend resolves its own through volk the same way.
    target_compile_definitions(fitzel_rhi_vulkan PRIVATE
        VMA_STATIC_VULKAN_FUNCTIONS=0
        VMA_DYNAMIC_VULKAN_FUNCTIONS=1
        IMGUI_IMPL_VULKAN_USE_VOLK)

    target_link_libraries(fitzel_rhi_factory PRIVATE fitzel_rhi_vulkan)
    target_compile_definitions(fitzel_rhi_factory PRIVATE FITZEL_WITH_VULKAN)
endif()

if(FITZEL_WITH_OPENGL)
    add_library(fitzel_rhi_opengl STATIC
        rhi/opengl/gl_device.cpp
        rhi/opengl/gl_command_list.cpp
        rhi/opengl/gl_factory.cpp)
    target_link_libraries(fitzel_rhi_opengl
        PUBLIC  fitzel_rhi
        PRIVATE fitzel_glad
                glfw
                fitzel_warnings)

    target_link_libraries(fitzel_rhi_factory PRIVATE fitzel_rhi_opengl)
    target_compile_definitions(fitzel_rhi_factory PRIVATE FITZEL_WITH_OPENGL)
endif()

add_library(fitzel_engine STATIC
    engine/application.cpp
    engine/camera.cpp
    engine/scene.cpp
    engine/scene_io.cpp
    engine/lsystem.cpp
    engine/animation.cpp
    engine/ui_theme.cpp
    engine/editor.cpp
    engine/editor_panels.cpp
    engine/editor_gizmo.cpp
    engine/editor_lighting.cpp
    engine/editor_lsystem.cpp
    engine/editor_timeline.cpp)
target_link_libraries(fitzel_engine
    PUBLIC  fitzel_rhi fitzel_platform
    PRIVATE imgui imguizmo nlohmann_json::nlohmann_json fitzel_warnings)

add_executable(fitzel main.cpp)
target_link_libraries(fitzel PRIVATE fitzel_engine fitzel_rhi_factory fitzel_warnings)

if(FITZEL_WITH_VULKAN)
    fitzel_stage_shaders(fitzel fitzel_shaders_vulkan)
endif()
if(FITZEL_WITH_OPENGL)
    fitzel_stage_shaders(fitzel fitzel_shaders_opengl)
endif()

# Stage the example scenes next to the executable, where the editor's Examples
# list looks and where relative --scene paths resolve.
add_custom_command(TARGET fitzel POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/scenes" "$<TARGET_FILE_DIR:fitzel>/scenes"
    COMMENT "Staging example scenes next to fitzel"
    VERBATIM)
