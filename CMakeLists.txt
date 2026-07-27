cmake_minimum_required(VERSION 4.2)
project(etsl)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(etsl main.cpp)

target_sources(etsl
    PUBLIC FILE_SET CXX_MODULES FILES
        src/reactor/reactor.cppm
        src/reactor/reactor_trait.cppm
        src/reactor/impl/iocp/reactor_iocp.cppm
        src/reactor/impl/iocp/reactor_iocp_defs.cppm
        src/platform/net/tcp_driver.cppm
        src/platform/net/tcp_driver_types.cppm
        src/platform/net/iocp/tcp_driver_iocp.cppm
        src/socket/socket_factory.cppm
        src/socket/socket_raii.cppm
        src/socket/socket_independent.cppm
        src/socket/tcp_socket.cppm
        src/socket/tcp_write_request.cppm
        src/win/wsa_initializer.cppm
        src/core/socket_types.cppm
        src/core/error.cppm
        src/timer/timer_bucket.cppm
        src/timer/timer.cppm
        src/timer/timer_types.cppm
        src/platform/net_types.cppm
        src/platform/net_ops.cppm
        src/socket/socket_address.cppm
        src/test/smoke_tcp_socket.cppm
    PRIVATE
        src/reactor/impl/iocp/reactor_iocp.cpp
        src/platform/net/iocp/tcp_driver_iocp.cpp
        src/win/wsa_initializer.cpp
        src/socket/socket_independent.cpp
        src/socket/tcp_socket.cpp
        src/timer/timer_bucket.cpp
        src/platform/etl_chrono.cpp
        src/platform/net_ops.cpp
        src/socket/socket_address.cpp
        src/test/smoke_tcp_socket.cpp
)

target_include_directories(etsl PUBLIC
        "."
        "src/"
        "external/"
        "external/etl/include/"
)

target_link_libraries(etsl PRIVATE ws2_32)

find_program(SIZE_TOOL size REQUIRED)
add_custom_target(size-report
    COMMAND ${SIZE_TOOL} -A $<TARGET_FILE:etsl>
    COMMENT "Size report (size -A)"
)
