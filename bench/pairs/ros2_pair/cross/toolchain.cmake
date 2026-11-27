# bench/pairs/ros2_pair/cross/toolchain.cmake: aarch64 from an amd64 noble, against
# the sysroot at /sysroot (an arm64 ros:jazzy-ros-base, unpacked). Programs (the
# rosidl generators, Python) are the build machine's; libraries, headers and CMake
# packages are the sysroot's only.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_SYSROOT /sysroot)
set(CMAKE_FIND_ROOT_PATH /sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
# The target's Python, for the Python typesupport the interface generators also build.
set(Python3_INCLUDE_DIR /sysroot/usr/include/python3.12)
set(Python3_LIBRARY /sysroot/usr/lib/aarch64-linux-gnu/libpython3.12.so)
set(Python3_NumPy_INCLUDE_DIR /sysroot/usr/lib/python3/dist-packages/numpy/core/include)
