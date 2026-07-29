cmake_minimum_required(VERSION 4.2)
project(etsl)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ETSL_BUILD_TESTS "Build etsl unit tests" ON)

include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

if(ETSL_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)

    # BMI from etsl_core is built without exceptions/RTTI — match gtest TUs.
    foreach(_gtest_target IN ITEMS gtest gtest_main)
        if(TARGET ${_gtest_target})
            target_compile_options(${_gtest_target} PRIVATE
                $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-exceptions -fno-rtti>
            )
        endif()
    endforeach()
endif()

add_library(etsl_core STATIC)

target_sources(etsl_core
    PUBLIC FILE_SET CXX_MODULES FILES
        src/reactor/reactor.cppm
        src/reactor/reactor_trait.cppm
        src/reactor/iocp/reactor_iocp.cppm
        src/reactor/iocp/reactor_iocp_defs.cppm
        src/platform/net/tcp_socket_driver.cppm
        src/platform/net/tcp_socket_driver_types.cppm
        src/platform/net/iocp/tcp_socket_driver_iocp_defs.cppm
        src/platform/net/iocp/tcp_socket_driver_iocp.cppm
        src/socket/socket_factory.cppm
        src/socket/socket_raii.cppm
        src/socket/socket_independent.cppm
        src/win/wsa_initializer.cppm
        src/core/socket_types.cppm
        src/core/error.cppm
        src/timer/timer_bucket.cppm
        src/timer/timer.cppm
        src/timer/timer_types.cppm
        src/platform/net_types.cppm
        src/platform/net_ops.cppm
        src/socket/socket_address.cppm
    PRIVATE
        src/reactor/iocp/reactor_iocp.cpp
        src/win/wsa_initializer.cpp
        src/socket/socket_independent.cpp
        src/timer/timer_bucket.cpp
        src/platform/etl_chrono.cpp
        src/platform/net_ops.cpp
        src/socket/socket_address.cpp
        src/platform/net/iocp/tcp_socket_driver_iocp.cpp
)

target_include_directories(etsl_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/etl/include"
)

target_compile_options(etsl_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-exceptions -fno-rtti>
)

target_link_libraries(etsl_core PUBLIC ws2_32)

add_executable(etsl main.cpp)
target_link_libraries(etsl PRIVATE etsl_core)
target_compile_options(etsl PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-exceptions -fno-rtti>
)

if(ETSL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

find_program(SIZE_TOOL size)
if(SIZE_TOOL)
    add_custom_target(size-report
        COMMAND ${SIZE_TOOL} -A $<TARGET_FILE:etsl>
        COMMENT "Size report (size -A)"
    )
endif()
