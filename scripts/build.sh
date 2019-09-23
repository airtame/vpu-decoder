#!/bin/bash
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd)/
CMAKE_ARGS=""

# Handle arguments if any
for arg in "$@"; do
    case $arg in
        -d|--debug)
            CMAKE_ARGS="$CMAKE_ARGS -DBUILD_DEBUG=1"
            ;;
        *)
            echo "Invalid argument"
            exit 1
            ;;
    esac
done


# Sometimes SYSROOT_PATH is used, sometimes ROOTFS_PATH. Make sure both work.
if [[ $ROOTFS_PATH && -z $SYSROOT_PATH ]]; then
    export SYSROOT_PATH="$ROOTFS_PATH"
fi

# AIRTAME_VERSION should be set to sha_XXXXXX in development.
if [[ -z $AIRTAME_VERSION ]]; then
    export AIRTAME_VERSION=sha_$(cd "${SCRIPT_DIR}/.."; git describe --abbrev=6 --dirty --always)
fi

# Add some extra cmake options when cross-compiling.
if [[ $TOOLCHAIN_PATH ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain.cmake"
fi

# Don't pollute the top-level dir with build files.
mkdir -p build
cd build
cmake $CMAKE_ARGS ..
cmake --build .
