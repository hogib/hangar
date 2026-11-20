# bench/ros2/hangar_ros2_bench: the ROS 2 side of hangar's benchmark.
# Built by bench/ros2/run.sh (colcon, or cmake alone) into a work directory;
# not part of hangar's own build. Needs a stock ros-jazzy-ros-base.
cmake_minimum_required(VERSION 3.16)
project(hangar_ros2_bench)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Fixed16.msg"
  "msg/Fixed256.msg"
  "msg/Fixed4k.msg"
  "msg/Fixed1m.msg"
)
rosidl_get_typesupport_target(hgb_typesupport ${PROJECT_NAME} "rosidl_typesupport_cpp")

# hgb_: the names stay under the 15 characters of /proc/<pid>/comm, which is
# how bench/procbench.py finds them.
foreach(exe talker listener composed)
  add_executable(hgb_${exe} src/${exe}.cpp)
  target_link_libraries(hgb_${exe} rclcpp::rclcpp "${hgb_typesupport}")
  install(TARGETS hgb_${exe} DESTINATION lib/${PROJECT_NAME})
endforeach()

ament_export_dependencies(rosidl_default_runtime)
ament_package()
