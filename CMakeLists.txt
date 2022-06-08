cmake_minimum_required(VERSION 3.15)

project(TimeStretch)

set(CMAKE_CXX_STANDARD 17)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
find_package (glog 0.6.0 REQUIRED)

add_executable(TimeStretchTest
        server_base/audio_utils.cc
        TimeStretchTest.cpp)

target_link_libraries(TimeStretchTest
        glog::glog
        sox)