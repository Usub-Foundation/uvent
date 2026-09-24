# FreeBSD amd64 cross toolchain: host clang + lld against an extracted base.txz
# (./usr/include, ./usr/lib, ./lib) at UVENT_FREEBSD_SYSROOT. Used to build the
# BSD (kqueue) port on Linux and run the binaries in a FreeBSD VM.
#   cmake -S . -B build-freebsd -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/freebsd-x86_64.cmake \
#         -DUVENT_FREEBSD_SYSROOT=/path/to/sysroot -DUVENT_BUILD_TESTS=ON
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSTEM_VERSION 14.5)

set(UVENT_FREEBSD_SYSROOT "$ENV{UVENT_FREEBSD_SYSROOT}" CACHE PATH "Extracted FreeBSD base.txz")
if (NOT UVENT_FREEBSD_SYSROOT)
    message(FATAL_ERROR "set -DUVENT_FREEBSD_SYSROOT=<dir with usr/include, usr/lib, lib>")
endif ()
# try_compile projects re-read this file: hand the cache variable through
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES UVENT_FREEBSD_SYSROOT)

set(CMAKE_SYSROOT ${UVENT_FREEBSD_SYSROOT})
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET x86_64-unknown-freebsd14.5)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-freebsd14.5)
set(CMAKE_ASM_COMPILER_TARGET x86_64-unknown-freebsd14.5)

set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -stdlib=libc++ -Wl,--allow-shlib-undefined")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -stdlib=libc++")

set(CMAKE_FIND_ROOT_PATH ${UVENT_FREEBSD_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
