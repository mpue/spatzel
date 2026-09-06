#!/usr/bin/env bash
# Configure and build fitzel inside the container.
#
# Used both as the image's default command and by `docker run` against a mounted
# working tree, so the two cannot drift apart.
#
# The build directory is `build-linux`, deliberately not `build`: the tree is
# usually mounted from a host that has its own `build/` full of MSVC or Xcode
# state, and a CMake cache from another generator is not something to discover
# halfway through a configure.
set -euo pipefail

SRC=${FITZEL_SRC:-/src}
BUILD=${FITZEL_BUILD:-${SRC}/build-linux}
CONFIG=${FITZEL_CONFIG:-Release}

if [ ! -f "${SRC}/CMakeLists.txt" ]; then
    echo "fitzel-build: no CMakeLists.txt in ${SRC}." >&2
    echo "              Mount the working tree there, e.g. -v \"\$PWD:/src\"." >&2
    exit 2
fi

echo "==> configure (${CONFIG}) -> ${BUILD}"
cmake -S "${SRC}" -B "${BUILD}" -G Ninja -DCMAKE_BUILD_TYPE="${CONFIG}" "$@"

# The seam check is the project's load-bearing invariant: no graphics API symbol
# above src/rhi/<backend>/. Run it first so a violation fails before the long
# part of the build rather than after it.
echo "==> check_seam"
cmake --build "${BUILD}" --target check_seam

echo "==> fitzel"
cmake --build "${BUILD}" --target fitzel --parallel "$(nproc)"

echo "==> done: ${BUILD}/bin/fitzel"
ls -l "${BUILD}/bin/fitzel"
