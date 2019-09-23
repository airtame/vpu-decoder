SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_SYSTEM_VERSION 1)
if (NOT "$ENV{TOOLCHAIN_PATH}" STREQUAL "")
    SET (CMAKE_C_COMPILER  $ENV{TOOLCHAIN_PATH}/bin/arm-linux-gnueabihf-gcc)
    SET (CMAKE_CXX_COMPILER $ENV{TOOLCHAIN_PATH}/bin/arm-linux-gnueabihf-g++)
endif ()
if (NOT "$ENV{SYSROOT_PATH}" STREQUAL "")
    SET(CMAKE_SYSROOT $ENV{SYSROOT_PATH})
    SET(CMAKE_FIND_ROOT_PATH $ENV{SYSROOT_PATH})
endif ()

SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)