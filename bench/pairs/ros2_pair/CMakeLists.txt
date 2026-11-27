# bench/pairs/ros2_pair: ROS 2's node pair for bench/pairs (README.md there).
# Built with colcon into a work directory by tools/baselines.sh; not part of
# hangar's own build. Needs a stock ros-jazzy-ros-base. pair.h is found one
# directory up, or where -DPAIR_INCLUDE_DIR says.
cmake_minimum_required(VERSION 3.16)
project(ros2_pair)

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
if(NOT PAIR_INCLUDE_DIR)
  set(PAIR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Pair32.msg"
  "msg/Pair256.msg"
  "msg/Pair4k.msg"
  "msg/Pair64k.msg"
  "msg/Pair1m.msg"
)
rosidl_get_typesupport_target(pair_typesupport ${PROJECT_NAME} "rosidl_typesupport_cpp")

add_executable(ros2_pair_node src/ros2_pair.cpp)
target_include_directories(ros2_pair_node PRIVATE "${PAIR_INCLUDE_DIR}")
string(TOUPPER "${CMAKE_BUILD_TYPE}" pair_bt)
target_compile_definitions(ros2_pair_node PRIVATE
  "PAIR_CFLAGS=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}: ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${pair_bt}} (${CMAKE_BUILD_TYPE})\"")
target_link_libraries(ros2_pair_node rclcpp::rclcpp "${pair_typesupport}")
install(TARGETS ros2_pair_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

ament_export_dependencies(rosidl_default_runtime)
ament_package()
