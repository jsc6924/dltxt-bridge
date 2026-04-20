cmake_minimum_required(VERSION 3.21)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json DLTXT_BRIDGE_VCPKG_MANIFEST)
string(JSON DLTXT_BRIDGE_PROJECT_VERSION GET ${DLTXT_BRIDGE_VCPKG_MANIFEST} version)

project(DltxtBridge VERSION ${DLTXT_BRIDGE_PROJECT_VERSION} LANGUAGES CXX)

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
set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL REQUIRED)

enable_testing()

configure_file(bridge_version.hpp.in bridge_version.hpp @ONLY)

add_executable(dltxt_bridge main.cpp)
set_target_properties(dltxt_bridge PROPERTIES OUTPUT_NAME dltxt_bridge_v2)
target_include_directories(dltxt_bridge PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_executable(dltxt_bridge_tests tests/protocol_tests.cpp)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json
  ${CMAKE_CURRENT_SOURCE_DIR}/bridge_http_proxy.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_tests.cpp
)

target_link_libraries(dltxt_bridge PRIVATE 
    Boost::headers
    Boost::system
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_link_libraries(dltxt_bridge_tests PRIVATE 
  Boost::headers
  Boost::system
  nlohmann_json::nlohmann_json
  OpenSSL::SSL
  OpenSSL::Crypto
)

file(STRINGS tests/protocol_tests.cpp DLTXT_BRIDGE_TEST_CASE_LINES
  REGEX "^[ 	]*DEFINE_TEST\\(test_[A-Za-z0-9_]+\\)[ 	]*\\{")

set(DLTXT_BRIDGE_TEST_CASES)
foreach(test_case_line IN LISTS DLTXT_BRIDGE_TEST_CASE_LINES)
  string(REGEX REPLACE "^[ 	]*DEFINE_TEST\\((test_[A-Za-z0-9_]+)\\)[ 	]*\\{$" "\\1" test_case "${test_case_line}")
  list(APPEND DLTXT_BRIDGE_TEST_CASES ${test_case})
endforeach()

foreach(test_case IN LISTS DLTXT_BRIDGE_TEST_CASES)
  add_test(NAME ${test_case} COMMAND dltxt_bridge_tests ${test_case})
endforeach()