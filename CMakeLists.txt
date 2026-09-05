cmake_minimum_required(VERSION 4.2)
project(etsl)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ETSL_BUILD_TESTS "Build etsl unit tests" OFF)

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

if(WIN32)
    target_compile_definitions(etsl_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_sources(etsl_core
    PUBLIC FILE_SET CXX_MODULES FILES
        src/etsl.cppm
        src/reactor/reactor.cppm
        src/reactor/reactor_trait.cppm
        src/reactor/iocp/reactor_iocp.cppm
        src/reactor/iocp/reactor_iocp_defs.cppm
        src/net/net.cppm
        src/net/net_defs.cppm
        src/net/net_ops.cppm
        src/net/net_address.cppm
        src/net/net_socket.cppm
        src/net/net_socket_factory.cppm
        src/tcp/connection/tcp_connection.cppm
        src/tcp/connection/tcp_connection_defs.cppm
        src/tcp/connection/tcp_connection_delegate_trait.cppm
        src/tcp/connection/iocp/tcp_connection_iocp_defs.cppm
        src/tcp/connection/iocp/tcp_connection_iocp.cppm
        src/tcp/acceptor/tcp_acceptor.cppm
        src/tcp/acceptor/tcp_acceptor_defs.cppm
        src/tcp/acceptor/tcp_acceptor_delegate_trait.cppm
        src/tcp/acceptor/iocp/tcp_acceptor_iocp_defs.cppm
        src/tcp/acceptor/iocp/tcp_acceptor_iocp.cppm
        src/timer/timer.cppm
        src/timer/timer_bucket.cppm
        src/timer/timer_impl.cppm
        src/timer/timer_defs.cppm
        src/init/initializer.cppm
    PRIVATE
        src/reactor/iocp/reactor_iocp.cpp
        src/timer/timer_bucket.cpp
        src/util/etl_chrono.cpp
        src/net/net_address.cpp
)

if(WIN32)
    target_sources(etsl_core PRIVATE
        src/init/win/win_initializer.cpp
        src/net/win/net_ops_win.cpp
        src/net/win/net_socket_factory_win.cpp
    )
endif()

target_include_directories(etsl_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/external"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/etl/include"
)

target_compile_options(etsl_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-exceptions -fno-rtti>
)

if(WIN32)
    target_link_libraries(etsl_core PUBLIC ws2_32)
endif()

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

#if(WIN32)
    add_custom_command(TARGET etsl POST_BUILD
        COMMAND ssh -i $ENV{HOME}/.ssh/win-dev
            sexey@192.168.122.51
            "cmd /c \"taskkill /IM gdbserver.exe /F >nul 2>&1 || exit /b 0\""
        COMMAND scp -i $ENV{HOME}/.ssh/win-dev
            $<TARGET_FILE:etsl>
            sexey@192.168.122.51:C:/dev/etsl.exe
        COMMENT "Deploying etsl.exe"
        VERBATIM
    )
#endif()