# syntax=docker/dockerfile:1

# The Linux build of fitzel, containerised.
#
# Two stages, because there are two things people want from this:
#
#   deps   — the toolchain and every development package the build needs, with
#            no source in it. Mount the working tree and build repeatedly; this
#            is the one to use while iterating.
#   build  — the same image with the tree copied in and compiled, so that
#            `docker build` itself is a verification that the Linux build works.
#            This is the default target.
#
# Neither stage runs the engine: that needs a GPU and a display, and the point
# of this image is the build. See the notes at the bottom for running it anyway.

# ---------------------------------------------------------------------------
# deps
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS deps

ENV DEBIAN_FRONTEND=noninteractive

# Ubuntu 24.04 carries CMake 3.28, comfortably past the 3.24 the project needs.
#
# Grouped by why they are here rather than alphabetically — a package list is
# only maintainable if the next person can tell which line to delete when a
# dependency goes away:
#
#   toolchain     compiler, CMake, Ninja, git (every dependency is FetchContent)
#   glad          generates its GL loader at configure time, in Python, with
#                 Jinja2 templates
#   shaders       glslc (shaderc) preferred, glslangValidator as the fallback —
#                 cmake/CompileShaders.cmake supports both, and spells the
#                 OpenGL target differently for each
#   vulkan        the loader; the headers come from FetchContent, but volk
#                 dlopens libvulkan.so.1 at run time
#   opengl        GL headers and libGL for the second backend
#   x11/wayland   GLFW's two window backends. Both are installed on purpose:
#                 the top-level CMakeLists falls back to X11 when the Wayland
#                 toolchain is incomplete, and an image that only ever exercised
#                 the fallback would not prove the Wayland path builds.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        ninja-build \
        pkg-config \
        python3 \
        python3-jinja2 \
        glslang-tools \
        libvulkan-dev \
        libgl1-mesa-dev \
        libx11-dev \
        libxrandr-dev \
        libxinerama-dev \
        libxcursor-dev \
        libxi-dev \
        libxkbcommon-dev \
        libwayland-dev \
        libwayland-bin \
        wayland-protocols \
        extra-cmake-modules \
 && (apt-get install -y --no-install-recommends glslc \
     || echo "note: no glslc package in this release; falling back to glslangValidator") \
 && rm -rf /var/lib/apt/lists/*

COPY docker/build.sh /usr/local/bin/fitzel-build
RUN chmod +x /usr/local/bin/fitzel-build

WORKDIR /src
CMD ["fitzel-build"]

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
FROM deps AS build

# Every dependency is fetched at configure time, so this layer needs network.
COPY . /src
RUN fitzel-build

# Usage
# -----
#   Build and verify in one go (default target):
#     docker build -t spatzel -f docker/Dockerfile .
#
#   Get the binary out:
#     id=$(docker create spatzel) \
#       && docker cp "$id:/src/build-linux/bin/fitzel" . \
#       && docker rm "$id"
#
#   Iterate against the working tree (nothing is copied; build-linux/ appears
#   in the tree and is gitignored, so it never collides with a host build):
#     docker build -t spatzel-deps --target deps -f docker/Dockerfile .
#     docker run --rm -v "$PWD:/src" spatzel-deps
#     docker run --rm -v "$PWD:/src" spatzel-deps fitzel-build -DFITZEL_WITH_OPENGL=OFF
#
#   Running the engine needs a GPU and a display, which the build does not:
#     docker run --rm -it \
#       --device /dev/dri \
#       -e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix \
#       -v "$PWD:/src" spatzel-deps \
#       /src/build-linux/bin/fitzel --scene scenes/dambreak.json
#   For Vulkan on NVIDIA, use the NVIDIA container runtime (--gpus all) and add
#   the vulkan ICD packages; Mesa (--device /dev/dri) covers AMD and Intel.
