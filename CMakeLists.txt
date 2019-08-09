cmake_minimum_required (VERSION 3.4)

project("examples")
set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS}-std=c++14)
set(CMAKE_BUILD_TYPE "Debug")
add_definitions(-DPGM_DEBUG)
include_directories(${PROJECT_SOURCE_DIR}/build/include/pgm-5.2)
link_directories(${PROJECT_SOURCE_DIR}/build/lib/)

set(common_lib
  pthread 
  pgm 
)

add_executable (receiver receiver.cpp)
add_executable (sender sender.cpp)

target_link_libraries(receiver ${common_lib})
target_link_libraries(sender ${common_lib})