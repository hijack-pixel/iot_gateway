cmake_minimum_required(VERSION 3.16)
project(iot_gateway CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(gateway src/gateway.cpp)
add_executable(device  src/device.cpp)
