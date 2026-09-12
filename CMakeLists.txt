cmake_minimum_required(VERSION 3.16)
project(iot_gateway CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)   # ★v8：面板用
qt_standard_project_setup()                             # 自动处理 Qt 的 moc

add_executable(gateway src/gateway.cpp)
add_executable(device  src/device.cpp)
add_executable(simulator src/simulator.cpp)
add_executable(panel    src/panel.cpp)                  # ★v8：监控面板

target_link_libraries(gateway PRIVATE pthread)
target_link_libraries(simulator PRIVATE pthread)
target_link_libraries(panel PRIVATE Qt6::Widgets Qt6::Network)
