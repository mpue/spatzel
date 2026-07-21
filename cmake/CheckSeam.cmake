# CheckSeam.cmake — enforces the RHI abstraction boundary.
#
# Run in script mode:
#   cmake -DSEAM_SOURCE_DIR=<repo root> -P cmake/CheckSeam.cmake
#
# Fails the build if any Vulkan symbol leaks into code above the backend
# boundary. The backend name in `rhi::Backend::Vulkan` is deliberately NOT
# matched — that enum is the one place an engine-level file may name a backend.

if(NOT DEFINED SEAM_SOURCE_DIR)
    message(FATAL_ERROR "CheckSeam: SEAM_SOURCE_DIR not set")
endif()

# Everything at or above the seam. src/rhi/<backend>/ are the only exceptions.
# The factory and the shared handle pool are scanned too: they sit beside the
# backends and must stay symbol-free.
set(SEAM_ROOTS
    "${SEAM_SOURCE_DIR}/src/engine"
    "${SEAM_SOURCE_DIR}/src/platform"
    "${SEAM_SOURCE_DIR}/src/rhi/rhi.hpp"
    "${SEAM_SOURCE_DIR}/src/rhi/handle_pool.hpp"
    "${SEAM_SOURCE_DIR}/src/rhi/factory.cpp"
    "${SEAM_SOURCE_DIR}/src/rhi/vulkan/vk_backend.hpp"
    "${SEAM_SOURCE_DIR}/src/rhi/opengl/gl_backend.hpp"
    "${SEAM_SOURCE_DIR}/src/main.cpp")

# Vulkan:
#   vk<Upper>  : vkCreateDevice, vkCmdDispatch, ...
#   Vk<Upper>  : VkDevice, VkImage, ...
#   VK_        : VK_SUCCESS, VK_FORMAT_*, VK_NO_PROTOTYPES, ...
#   Vma / vma  : VMA allocator types and functions
# OpenGL:
#   gl<Upper>  : glDispatchCompute, glBindImageTexture, ...
#   GL_        : GL_RGBA16F, GL_COMPUTE_SHADER, ...
#   GL<type>   : spelled out, because GL[A-Z] would also match every GLFW_*
#                constant and GLFW is a windowing library, not a graphics API.
set(_patterns
    "vk[A-Z]"
    "Vk[A-Z]"
    "VK_"
    "[Vv]ma[A-Z]"
    "VMA_"
    "include[ \t]*[<\"]volk"
    "include[ \t]*[<\"]vulkan"
    "gl[A-Z]"
    "(^|[^A-Za-z0-9_])GL_"
    "GL(uint|int|enum|sizei|float|double|bitfield|char|boolean|void|byte|short)"
    "GLAD"
    "include[ \t]*[<\"]glad"
)

set(_violations "")

foreach(_root IN LISTS SEAM_ROOTS)
    if(NOT EXISTS "${_root}")
        continue()
    endif()
    if(IS_DIRECTORY "${_root}")
        file(GLOB_RECURSE _files "${_root}/*.hpp" "${_root}/*.h"
                                 "${_root}/*.cpp" "${_root}/*.cc")
    else()
        set(_files "${_root}")
    endif()

    foreach(_file IN LISTS _files)
        file(STRINGS "${_file}" _lines)
        set(_lineno 0)
        foreach(_line IN LISTS _lines)
            math(EXPR _lineno "${_lineno} + 1")
            foreach(_pattern IN LISTS _patterns)
                if(_line MATCHES "${_pattern}")
                    list(APPEND _violations "  ${_file}:${_lineno}: ${_line}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()

if(_violations)
    list(REMOVE_DUPLICATES _violations)
    string(REPLACE ";" "\n" _report "${_violations}")
    message(FATAL_ERROR
        "RHI seam violated — graphics API symbols found above the backend boundary:\n"
        "${_report}\n"
        "A graphics API may only appear inside src/rhi/<backend>/.")
endif()

message(STATUS "RHI seam check: clean")
