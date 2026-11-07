# tools/sim/Dockerfile: the drivable ArduSub demo. Gazebo Harmonic, ArduSub SITL and
# the ardupilot_gazebo plugin in one Ubuntu 24.04 image; a BlueROV2 model from
# bluerov2_gz. Built by run.sh build, run by run.sh up; the host steps are in SETUP.md.
#
# Every external detail was checked against upstream on 2026-11-07:
#   Gazebo repo and package   https://gazebosim.org/docs/harmonic/install_ubuntu
#   ArduPilot tag, waf, prereqs  github.com/ArduPilot/ardupilot at the tag below
#                             (BUILD.md; Tools/environment_install/install-prereqs-ubuntu.sh)
#   ardupilot_gazebo          its README.md at the commit below ("Harmonic (apt)",
#                             "Clone the repo and build", "Configure"); CI builds it on
#                             ubuntu-24.04 with GZ_VERSION=harmonic
#   BlueROV2 model and worlds github.com/clydemcqueen/bluerov2_ignition, README
#                             "BlueROV2 in Gazebo Harmonic" (package name bluerov2_gz, MIT)
#   NVIDIA runtime variables  docs.nvidia.com/datacenter/cloud-native/container-toolkit/
#                             latest/docker-specialized.html

FROM ubuntu:24.04

# ArduPilot: Sub-4.7.2 is the commit the moving tag ArduSub-stable pointed at on
# 2026-11-07 (fb773567..., ArduSub/version.h "ArduSub V4.7.2"). A fixed tag, so the
# image is reproducible; ArduSub-stable moves.
ARG ARDUPILOT_REF=Sub-4.7.2
# ardupilot_gazebo main on 2026-11-07.
ARG ARDUPILOT_GAZEBO_REF=f116b03552d05c8d0b0bed5ed67033a1183121d1
# bluerov2_ignition main on 2026-11-07 (last commit 2026-10-10).
ARG BLUEROV2_GZ_REF=4451a5d1b15d204581d43ef7d93df4a0508eddbb
ARG DEBIAN_FRONTEND=noninteractive

# Tools the rest needs: the prereqs script wants sudo and a non-root user; lsb-release,
# curl and gnupg are what the Gazebo install page asks for; tmux runs the processes.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg lsb-release sudo git tmux locales \
        build-essential cmake pkg-config python3 python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Gazebo Harmonic from the OSRF repository, the install page's commands verbatim
# (run as root here, so without sudo), then the ardupilot_gazebo build dependencies
# from its README, "Harmonic (apt)". Harmonic binaries exist for Noble (24.04).
RUN curl https://packages.osrfoundation.org/gazebo.gpg --output /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] https://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/gazebo-stable.list \
    && apt-get update \
    && apt-get install -y gz-harmonic \
    && apt-get install -y libgz-sim8-dev rapidjson-dev \
    && apt-get install -y libopencv-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-gl \
    && rm -rf /var/lib/apt/lists/*

# OpenGL for the NVIDIA container runtime. With NVIDIA_DRIVER_CAPABILITIES including
# "graphics" the runtime mounts the host driver's libGLX_nvidia, libEGL_nvidia,
# libnvidia-glcore and the glvnd/Vulkan vendor files (libnvidia-container
# src/nvc_info.c; nvidia-container-toolkit internal/discover/graphics.go). There is no
# libnvidia-gl package to install: the driver comes from the host. The container
# needs the glvnd dispatch libraries those plug into (libglvnd0 libgl1 libglx0 libegl1
# libgles2, as NVIDIA's own cudagl images install), mesa-utils for glxinfo, the Mesa
# DRI drivers as the fallback when the GPU is absent, the Vulkan loader, and
# python3-wxgtk4.0 for MAVProxy's --console (the venv below sees system packages).
RUN apt-get update && apt-get install -y --no-install-recommends \
        libglvnd0 libgl1 libglx0 libegl1 libgles2 libxext6 libx11-6 \
        mesa-utils libgl1-mesa-dri libvulkan1 \
        python3-wxgtk4.0 \
    && rm -rf /var/lib/apt/lists/*
# "display" implies "graphics" (X11/Wayland windows); "utility" gives nvidia-smi.
# The variable replaces the default set, so everything needed is listed.
ENV NVIDIA_VISIBLE_DEVICES=all \
    NVIDIA_DRIVER_CAPABILITIES=graphics,display,utility,compute \
    QT_X11_NO_MITSHM=1

# A non-root user with uid 1000, the host user's, so files on the bind mounts are
# the host user's. The ubuntu:24.04 image already has a user "ubuntu" at uid 1000
# (community reports; the removal is harmless when it is absent). The prereqs
# script refuses to run as root and uses sudo, so sudo is passwordless for sim.
RUN if id ubuntu >/dev/null 2>&1; then userdel ubuntu && rm -rf /home/ubuntu; fi \
    && groupadd --gid 1000 sim \
    && useradd --uid 1000 --gid 1000 --create-home --shell /bin/bash sim \
    && echo 'sim ALL=(root) NOPASSWD:ALL' > /etc/sudoers.d/sim \
    && chmod 0440 /etc/sudoers.d/sim
USER sim
ENV USER=sim HOME=/home/sim
WORKDIR /home/sim

# ArduPilot at the pinned tag, with submodules (waf, mavlink, ...). Shallow: the
# history is not needed and the full tree is large.
RUN git clone --branch "${ARDUPILOT_REF}" --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/ArduPilot/ardupilot /home/sim/ardupilot

# The prereqs script, non-interactive. Its switches (read from the script at the tag):
#   -y                     assume yes (apt --assume-yes, no prompts)
#   SKIP_AP_GRAPHIC_ENV=1  no xterm, SFML, wx build deps: the GUI parts of SITL
#   SKIP_AP_EXT_ENV=1      no pygame/intelhex
#   SKIP_AP_COV_ENV=1      no lcov/gcovr
#   SKIP_AP_GIT_CHECK=1    no submodule update (the clone above did it)
#   DO_AP_STM_ENV=0        no STM32 cross toolchain (with -y the prompt would say yes)
#   DO_PYTHON_VENV_ENV=1   on Noble it creates $HOME/venv-ardupilot
#                          (--system-site-packages) and installs MAVProxy, pymavlink,
#                          empy, pexpect... into it; 1 writes the activate line into
#                          the login file
#   AP_DOCKER_BUILD=1      in Docker it writes ~/.ardupilot_env and sources it from
#                          ~/.bashrc instead of touching ~/.profile
# It also adds sim to dialout (harmless here).
WORKDIR /home/sim/ardupilot
RUN SKIP_AP_GRAPHIC_ENV=1 SKIP_AP_EXT_ENV=1 SKIP_AP_COV_ENV=1 SKIP_AP_GIT_CHECK=1 \
    DO_AP_STM_ENV=0 DO_PYTHON_VENV_ENV=1 AP_DOCKER_BUILD=1 \
    Tools/environment_install/install-prereqs-ubuntu.sh -y \
    && sudo rm -rf /var/lib/apt/lists/*

# The venv's python and sim_vehicle.py on PATH for every later step and for run.sh's
# docker exec (which does not read .bashrc). ccache as the prereqs script sets it.
ENV PATH=/home/sim/venv-ardupilot/bin:/home/sim/ardupilot/Tools/autotest:/usr/lib/ccache:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# SITL build (BUILD.md: "./waf configure --board sitl", "./waf sub  # ROV and other
# submarines"). Result: build/sitl/bin/ardusub. waf records the environment; the
# runtime environment differs, so launch.sh passes -N (--no-rebuild) to sim_vehicle.py.
RUN ./waf configure --board sitl && ./waf sub

# ardupilot_gazebo against Harmonic (README: GZ_VERSION set to harmonic; "cmake ..
# -DCMAKE_BUILD_TYPE=RelWithDebInfo"; "make -j4", here -j nproc). The plugin is
# build/libArduPilotPlugin.so.
WORKDIR /home/sim
ENV GZ_VERSION=harmonic
RUN git clone https://github.com/ArduPilot/ardupilot_gazebo /home/sim/ardupilot_gazebo \
    && git -C /home/sim/ardupilot_gazebo checkout --detach "${ARDUPILOT_GAZEBO_REF}" \
    && mkdir /home/sim/ardupilot_gazebo/build \
    && cmake -S /home/sim/ardupilot_gazebo -B /home/sim/ardupilot_gazebo/build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && make -C /home/sim/ardupilot_gazebo/build -j"$(nproc)"

# The BlueROV2 (base and Heavy) for Gazebo Harmonic. Its models carry the
# ArduPilotPlugin block (fdm_port_in 9002, lock_step, six or eight thruster channels)
# and use Gazebo's own buoyancy, hydrodynamics and thruster systems; no ROS.
RUN git clone https://github.com/clydemcqueen/bluerov2_ignition /home/sim/bluerov2_gz \
    && git -C /home/sim/bluerov2_gz checkout --detach "${BLUEROV2_GZ_REF}"

# Gazebo finds the plugin and the models through these (ardupilot_gazebo README,
# "Configure"; bluerov2_gz README, "Running Gazebo").
ENV GZ_SIM_SYSTEM_PLUGIN_PATH=/home/sim/ardupilot_gazebo/build \
    GZ_SIM_RESOURCE_PATH=/home/sim/bluerov2_gz/models:/home/sim/bluerov2_gz/worlds:/home/sim/ardupilot_gazebo/models:/home/sim/ardupilot_gazebo/worlds

# The worlds include one model from Fuel (OpenRobotics/models/Sun); fetch it into the
# image's cache (~/.gz/fuel) so the first launch does not wait on the network.
# (gz fuel download -u <url>: the fuel_tools command-line page shows the form with a
# world URL; a model URL is the same form.)
RUN gz fuel download -u https://fuel.gazebosim.org/1.0/OpenRobotics/models/Sun

# Logs and anything the demo writes go here; run.sh bind-mounts tools/sim/work on it.
RUN mkdir -p /home/sim/work
VOLUME /home/sim/work

# run.sh starts the container with "sleep infinity" and runs launch.sh in it.
CMD ["sleep", "infinity"]
