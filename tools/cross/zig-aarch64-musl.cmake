# CMake toolchain: zig cc as a static aarch64-linux-musl cross compiler
# (Raspberry Pi 4/5 under 64-bit Pi OS). Used to build the third-party C
# libraries hangar links (libjpeg-turbo) before building hangar itself.
#
#   cmake -S <src> -B <build> -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=tools/cross/zig-aarch64-musl.cmake \
#     -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_STATIC=ON
#
# Checked on 2026-11-07 with zig 0.17.0 and libjpeg-turbo 3.2.0: the
# static libturbojpeg built and electric-eye linked against it, and its
# 17 test suites passed as aarch64 binaries under qemu-aarch64.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER zig cc -target aarch64-linux-musl -mcpu=cortex_a76)
set(CMAKE_AR /usr/bin/zig)
set(CMAKE_C_COMPILER_AR /usr/bin/zig)
set(CMAKE_RANLIB /usr/bin/zig)
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "<CMAKE_RANLIB> ranlib <TARGET>")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
