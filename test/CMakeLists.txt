include(GoogleTest)

if(NOT WIN32)
    message(STATUS "etsl tests: C_TCPConnectionIOCP suite skipped (Windows/IOCP only)")
    return()
endif()

add_executable(tcp_connection_iocp_test
    tcp_connection_iocp_test.cpp
)

target_link_libraries(tcp_connection_iocp_test
    PRIVATE
        etsl_core
        GTest::gtest_main
)

target_compile_options(tcp_connection_iocp_test PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-fno-exceptions -fno-rtti>
)

if(WIN32)
    target_link_libraries(tcp_connection_iocp_test PRIVATE ws2_32)
endif()

# Cross builds cannot execute the PE at configure/build time.
if(CMAKE_CROSSCOMPILING)
    add_test(NAME tcp_connection_iocp_test COMMAND tcp_connection_iocp_test)
else()
    gtest_discover_tests(tcp_connection_iocp_test)
endif()
