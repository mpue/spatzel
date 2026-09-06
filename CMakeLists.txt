cmake_minimum_required(VERSION 3.24)

project(fitzel
    VERSION 0.1.0
    DESCRIPTION "Portable engine base with a swappable graphics backend"
    # C is needed for the third-party dependencies pulled in below (GLFW, volk,
    # the glad loader). The Visual Studio generator enables C implicitly, but
    # single-config generators (Ninja, Make) do not, so it must be explicit.
    LANGUAGES C CXX)

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
# Backends
#
# Which backends exist in the binary is decided here; which one runs is decided
# at startup by --backend. Turning one off must never break the other.
# ---------------------------------------------------------------------------
option(FITZEL_WITH_VULKAN "Build the Vulkan backend" ON)
option(FITZEL_WITH_OPENGL "Build the OpenGL backend" ON)

if(NOT FITZEL_WITH_VULKAN AND NOT FITZEL_WITH_OPENGL)
    message(FATAL_ERROR "At least one backend must be enabled.")
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

# --- GLFW's window backends on Linux ---------------------------------------
#
# GLFW 3.4 builds X11 *and* Wayland by default, and the Wayland half needs three
# things at configure time that a plain toolchain does not have: the
# wayland-scanner code generator, the wayland-protocols XML, and
# extra-cmake-modules. Missing any of them fails the configure deep inside GLFW
# with a message naming wayland-scanner and nothing else — which does not say
# what to install, and does not say that X11 alone would have worked.
#
# So probe for the three, and fall back to X11 with an explanation rather than an
# error. Falling back is safe even on a Wayland-only desktop: GLFW's X11 backend
# runs under XWayland. Both are overridable (-DFITZEL_GLFW_WAYLAND=OFF) for
# anyone who wants to make the choice themselves.
if(UNIX AND NOT APPLE)
    option(FITZEL_GLFW_X11     "Build GLFW's X11 backend"     ON)
    option(FITZEL_GLFW_WAYLAND "Build GLFW's Wayland backend" ON)

    set(FITZEL_GLFW_WAYLAND_ENABLED ${FITZEL_GLFW_WAYLAND})

    if(FITZEL_GLFW_WAYLAND_ENABLED)
        find_program(FITZEL_WAYLAND_SCANNER NAMES wayland-scanner)

        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FITZEL_WAYLAND_PROTOCOLS QUIET "wayland-protocols>=1.15")
        endif()

        # GLFW's own Wayland path does find_package(ECM REQUIRED NO_MODULE).
        find_package(ECM QUIET NO_MODULE)

        set(FITZEL_WAYLAND_MISSING "")
        if(NOT FITZEL_WAYLAND_SCANNER)
            list(APPEND FITZEL_WAYLAND_MISSING "wayland-scanner")
        endif()
        if(NOT FITZEL_WAYLAND_PROTOCOLS_FOUND)
            list(APPEND FITZEL_WAYLAND_MISSING "wayland-protocols (>= 1.15)")
        endif()
        if(NOT ECM_FOUND)
            list(APPEND FITZEL_WAYLAND_MISSING "extra-cmake-modules")
        endif()

        if(FITZEL_WAYLAND_MISSING)
            list(JOIN FITZEL_WAYLAND_MISSING ", " FITZEL_WAYLAND_MISSING_TEXT)
            message(STATUS
                "fitzel: GLFW Wayland backend disabled - missing ${FITZEL_WAYLAND_MISSING_TEXT}. "
                "Building X11 only (which runs under XWayland). To enable Wayland, install: "
                "Debian/Ubuntu 'libwayland-bin wayland-protocols extra-cmake-modules libwayland-dev'; "
                "Fedora 'wayland-devel wayland-protocols-devel extra-cmake-modules'; "
                "Arch 'wayland wayland-protocols extra-cmake-modules'.")
            set(FITZEL_GLFW_WAYLAND_ENABLED OFF)
        endif()
    endif()

    if(NOT FITZEL_GLFW_X11 AND NOT FITZEL_GLFW_WAYLAND_ENABLED)
        message(FATAL_ERROR
            "fitzel: no GLFW window backend left to build. Enable one of "
            "FITZEL_GLFW_X11 / FITZEL_GLFW_WAYLAND and install its development "
            "packages (X11 needs e.g. 'libx11-dev libxrandr-dev libxinerama-dev "
            "libxcursor-dev libxi-dev' on Debian/Ubuntu).")
    endif()

    set(GLFW_BUILD_X11     ${FITZEL_GLFW_X11}             CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND ${FITZEL_GLFW_WAYLAND_ENABLED} CACHE BOOL "" FORCE)
endif()

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

FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.8
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  cmake)

# Dear ImGui ships no CMake of its own; we compile the core and the platform
# backend into libraries of our own below. The graphics-API render backends are
# compiled per RHI backend, in src/, so no imgui_impl_<api> ever crosses the
# seam.
# Docking branch: the DockSpace / DockBuilder APIs and ImGuiConfigFlags_Docking
# live only here, not in the tagged releases. Pinned to a specific docking commit
# (not shallow: a bare SHA off a moving branch cannot be fetched shallowly).
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        b334d19b667958ed970000073644d911fae17e57)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    ON)

# ImGuizmo: the transform gizmo (translate/rotate/scale) for the editor. It sits
# on top of ImGui — pure ImGui:: / ImGuizmo:: symbols, no graphics API — so it
# stays above the RHI seam. Pinned to a commit verified against ImGui v1.91.8
# (it includes imgui_internal.h, so version skew matters). Not shallow: a bare
# SHA cannot be fetched shallowly.
FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        dc25afb98bc3ebe00dfc9a23ba7235fead2ccb1d
    # ImGuizmo ships its own CMakeLists that declares a conflicting target and
    # cannot find our ImGui; point SOURCE_SUBDIR at a directory it does not have
    # so MakeAvailable only downloads the sources — we compile them ourselves.
    SOURCE_SUBDIR  __fitzel_no_cmake__)

FetchContent_MakeAvailable(glfw)

if(FITZEL_WITH_VULKAN)
    FetchContent_MakeAvailable(vulkan_headers)

    # volk and vk-bootstrap resolve Vulkan headers through these.
    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
    set(VK_BOOTSTRAP_TEST   OFF CACHE BOOL "" FORCE)
    set(VK_BOOTSTRAP_WERROR OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(volk vma vk_bootstrap)

    target_link_libraries(volk PUBLIC Vulkan::Headers)
    target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
endif()

if(FITZEL_WITH_OPENGL)
    # glad2 generates its loader at configure time (needs Python 3).
    FetchContent_MakeAvailable(glad)
    glad_add_library(fitzel_glad REPRODUCIBLE API gl:core=4.6)
endif()

# ---------------------------------------------------------------------------
# Editor UI dependencies
#
# Dear ImGui for the panel, nlohmann/json for human-readable scene files. The
# ImGui core is backend-neutral (its symbols are plain `ImGui::`); only the
# render backends are API-specific, and those are compiled inside
# src/rhi/<backend>/, never above the seam. The GLFW platform backend touches
# only windowing, so it lives with the platform layer.
# ---------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(imgui nlohmann_json imguizmo)

# ImGui core. Third-party, so it is kept off fitzel_warnings.
add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
target_include_directories(imgui PUBLIC
    "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")

# ImGuizmo core, compiled against our ImGui. Third-party, so also off
# fitzel_warnings. Links only ImGui, so nothing here reaches the seam.
add_library(imguizmo STATIC
    "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
target_include_directories(imguizmo PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(imguizmo PUBLIC imgui)

# The GLFW platform backend for ImGui. Pure windowing glue — no graphics API —
# so it belongs beside the platform layer, not behind the seam.
add_library(imgui_glfw STATIC
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp")
target_link_libraries(imgui_glfw PUBLIC imgui glfw)

# Path to the ImGui render backends, so each RHI backend can compile its own.
set(FITZEL_IMGUI_BACKENDS_DIR "${imgui_SOURCE_DIR}/backends")

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
