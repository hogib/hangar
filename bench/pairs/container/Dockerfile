# bench/pairs/container/Dockerfile: where tools/baselines.sh builds the baselines that
# cannot be static musl programs: the ROS 2 node (it links the installed ROS 2), LCM
# (it links GLib) and MOOS (C++). An amd64 Ubuntu 24.04 with ROS 2 Jazzy, Ubuntu's
# aarch64 cross compiler, and GLib's development files for both architectures, so that
# what is built here links what the Pi has: the same distribution, the same glibc,
# libstdc++ and GLib.
#
#   docker build -t hangar-baselines-build:noble bench/pairs/container
FROM ros:jazzy-ros-base
RUN sed -i 's/^Types: deb$/Types: deb\nArchitectures: amd64/' /etc/apt/sources.list.d/ubuntu.sources \
 && sed -i 's/^Types: deb deb-src$/Types: deb\nArchitectures: amd64/' /etc/apt/sources.list.d/ros2.sources \
 && printf 'Types: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports\nSuites: noble noble-updates noble-security\nComponents: main universe\nArchitectures: arm64\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' \
      > /etc/apt/sources.list.d/ubuntu-ports.sources \
 && dpkg --add-architecture arm64 \
 && apt-get update -q \
 && apt-get install -y -q --no-install-recommends \
      g++-aarch64-linux-gnu libglib2.0-dev libglib2.0-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*
