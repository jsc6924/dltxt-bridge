cmake_minimum_required(VERSION 3.21)
project(DltxtBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# Add /bigobj flag for MSVC to handle large object files from Boost templates
if(MSVC)
  add_compile_options(/bigobj)
endif()

# Find dependencies via vcpkg
find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json CONFIG REQUIRED)

enable_testing()

add_executable(dltxt_bridge main.cpp)

add_executable(dltxt_bridge_tests tests/protocol_tests.cpp)

target_link_libraries(dltxt_bridge PRIVATE 
    Boost::headers
    Boost::system
    nlohmann_json::nlohmann_json
)

target_link_libraries(dltxt_bridge_tests PRIVATE 
  Boost::headers
  Boost::system
  nlohmann_json::nlohmann_json
)

add_test(NAME dltxt_bridge_tests COMMAND dltxt_bridge_tests)