#!/bin/bash
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd)/
CMAKE_ARGS=""

# Handle arguments if any
for arg in "$@"; do
    case $arg in
        -d|--debug)
            CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_BUILD_TYPE=Debug"
            ;;
        *)
            echo "Invalid argument"
            exit 1
            ;;
    esac
done

TOOLCHAIN=$AIRTAME_TOOLCHAIN
SYSROOT=$AIRTAME_SYSROOT
test -z $AIRTAME_TOOLCHAIN && TOOLCHAIN=$TOOLCHAIN_PATH
test -z $AIRTAME_SYSROOT && SYSROOT=$SYSROOT_PATH


# Sometimes AIRTAME_SYSROOT is used, sometimes ROOTFS_PATH. Make sure both work.
if [[ $ROOTFS_PATH && -z $SYSROOT ]]; then
    SYSROOT="$ROOTFS_PATH"
fi

# AIRTAME_VERSION should be set to sha_XXXXXX in development.
if [[ -z $AIRTAME_VERSION ]]; then
    export AIRTAME_VERSION=sha_$(cd "${SCRIPT_DIR}/.."; git describe --abbrev=6 --dirty --always)
fi

# Add some extra cmake options when cross-compiling.
if [[ $TOOLCHAIN ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain.cmake \
                            -DAIRTAME_TOOLCHAIN=$TOOLCHAIN -DAIRTAME_SYSROOT=$SYSROOT"
fi

# Don't pollute the top-level dir with build files.
mkdir -p build
cd build
cmake $CMAKE_ARGS ..
cmake --build .
