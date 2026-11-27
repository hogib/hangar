# bench/pairs/ros2_pair/cross/Dockerfile: the amd64 ROS 2 Jazzy image with Ubuntu's
# aarch64 cross compiler added, for tools/baselines.sh --ros2-cross: the ROS 2 node
# built for the Pi on a laptop, against an arm64 ROS 2 Jazzy unpacked as a sysroot.
# Same distribution on both sides (noble), so the same glibc and libstdc++ as the Pi's.
#
#   docker build -t hangar-ros2-cross:jazzy bench/pairs/ros2_pair/cross
FROM ros:jazzy-ros-base
RUN apt-get update -q \
 && apt-get install -y -q --no-install-recommends g++-aarch64-linux-gnu \
 && rm -rf /var/lib/apt/lists/*
