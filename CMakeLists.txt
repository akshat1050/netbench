cmake_minimum_required(VERSION 3.20)
project(netbench VERSION 0.2.0 LANGUAGES CXX)

option(NETBENCH_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(netbench_lib
    src/core/config.cpp
    src/core/metrics.cpp
    src/core/rate_limiter.cpp
    src/core/socket.cpp
    src/engines/tcp_engine.cpp
    src/engines/udp_engine.cpp
    src/engines/icmp_engine.cpp
    src/reporting/terminal.cpp)
target_include_directories(netbench_lib PUBLIC include)
target_compile_features(netbench_lib PUBLIC cxx_std_20)
target_compile_options(netbench_lib PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_link_libraries(netbench_lib PUBLIC pthread)

if(NETBENCH_ENABLE_SANITIZERS)
  target_compile_options(netbench_lib PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(netbench_lib PUBLIC -fsanitize=address,undefined)
endif()

add_executable(netbench src/main.cpp)
target_link_libraries(netbench PRIVATE netbench_lib)

include(CTest)
if(BUILD_TESTING)
  add_executable(netbench_unit_tests tests/unit_tests.cpp)
  target_link_libraries(netbench_unit_tests PRIVATE netbench_lib)
  add_test(NAME unit COMMAND netbench_unit_tests)

  add_executable(netbench_protocol_integration_test tests/protocol_integration_test.cpp)
  target_link_libraries(netbench_protocol_integration_test PRIVATE netbench_lib)
  add_test(NAME protocols_localhost COMMAND netbench_protocol_integration_test)
  set_tests_properties(protocols_localhost PROPERTIES TIMEOUT 15)
endif()

