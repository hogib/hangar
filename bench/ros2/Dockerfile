# bench/ros2/Dockerfile: ROS 2 Jazzy with the three middlewares run.sh knows,
# for a machine that has no ROS 2 of its own (run.sh --docker). A smoke test
# of the benchmark, or a comparison on a machine that must stay clean; the
# numbers that go into docs/00 come from a native ROS 2 on the Pi.
#
#   docker build -t hangar-ros2-bench:jazzy bench/ros2
#   bench/ros2/run.sh --docker hangar-ros2-bench:jazzy --quick
FROM ros:jazzy-ros-base
RUN apt-get update -q \
 && apt-get install -y -q --no-install-recommends \
      ros-jazzy-rmw-cyclonedds-cpp ros-jazzy-rmw-zenoh-cpp \
 && rm -rf /var/lib/apt/lists/*
