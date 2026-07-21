cmake_minimum_required(VERSION 3.24)

project(fitzel
    VERSION 0.1.0
    DESCRIPTION "Portable engine base with a swappable graphics backend"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Several upstream dependencies still declare cmake_minimum_required(VERSION 3.x)
# below what CMake 4 accepts. This keeps them configurable without patching.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Warning flags apply to our own targets only — third-party sources pulled in by
# FetchContent must not be able to break the build.
add_library(fitzel_warnings INTERFACE)
if(MSVC)
    target_compile_options(fitzel_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(fitzel_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        vulkan-sdk-1.4.350.1
    GIT_SHALLOW    ON)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    ON)

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.4.350.1
    GIT_SHALLOW    ON)

FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    ON)

FetchContent_Declare(vk_bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    # Must not be newer than the Vulkan-Headers tag above — vk-bootstrap's
    # dispatch table references entry points from the header revision it was
    # generated against.
    GIT_TAG        v1.4.350
    GIT_SHALLOW    ON)

FetchContent_MakeAvailable(vulkan_headers glfw)

# volk and vk-bootstrap resolve Vulkan headers through these.
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_TEST   OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_WERROR OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(volk vma vk_bootstrap)

target_link_libraries(volk PUBLIC Vulkan::Headers)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)

# ---------------------------------------------------------------------------
# Project targets
# ---------------------------------------------------------------------------
# Included here rather than in shaders/ so the helper functions are visible to
# every subdirectory.
include(CompileShaders)

add_subdirectory(shaders)
add_subdirectory(src)

# ---------------------------------------------------------------------------
# RHI seam enforcement — everything above the backend boundary must be free of
# Vulkan symbols. Wired as a build-time dependency of the executable.
# ---------------------------------------------------------------------------
add_custom_target(check_seam ALL
    COMMAND "${CMAKE_COMMAND}"
            "-DSEAM_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSeam.cmake"
    COMMENT "Checking RHI seam"
    VERBATIM)
