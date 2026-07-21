# Target layout mirrors the architecture:
#
#   fitzel_platform    windowing/input/timing        (glfw only)
#   fitzel_rhi         the seam, header-only         (no dependencies at all)
#   fitzel_rhi_vulkan  the only Vulkan-aware target  (volk, VMA, vk-bootstrap)
#   fitzel_engine      frame orchestration           (rhi + platform, NOT the backend)
#   fitzel             executable, links a backend in
#
# fitzel_engine deliberately does not link fitzel_rhi_vulkan. Swapping backends
# is an edit to the executable's link line plus one enum value.

add_library(fitzel_platform STATIC
    platform/window.cpp)
target_include_directories(fitzel_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fitzel_platform PUBLIC glfw PRIVATE fitzel_warnings)

add_library(fitzel_rhi INTERFACE)
target_include_directories(fitzel_rhi INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(fitzel_rhi_vulkan STATIC
    rhi/vulkan/vk_common.cpp
    rhi/vulkan/vk_swapchain.cpp
    rhi/vulkan/vk_device.cpp
    rhi/vulkan/vk_command_list.cpp
    rhi/vulkan/vk_factory.cpp)
target_link_libraries(fitzel_rhi_vulkan
    PUBLIC  fitzel_rhi
    PRIVATE volk
            GPUOpen::VulkanMemoryAllocator
            vk-bootstrap::vk-bootstrap
            glfw
            fitzel_warnings)
# volk owns every entry point, so VMA must resolve its own through the two
# loader functions it is handed at allocator creation.
target_compile_definitions(fitzel_rhi_vulkan PRIVATE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

add_executable(fitzel main.cpp)
target_link_libraries(fitzel PRIVATE fitzel_platform fitzel_rhi fitzel_rhi_vulkan fitzel_warnings)
