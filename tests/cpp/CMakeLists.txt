cmake_minimum_required(VERSION 3.16)
project(openppp2_cpp_tests LANGUAGES CXX)

option(ENABLE_COVERAGE "Enable LLVM coverage instrumentation" OFF)

get_filename_component(OPENPPP2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

find_package(OpenSSL REQUIRED)

set(OPENPPP2_TEST_INCLUDES
    "${OPENPPP2_ROOT}"
    "${OPENPPP2_ROOT}/common"
    "${OPENPPP2_ROOT}/common/dnslib"
)

foreach(_hint /usr/include /usr/local/include /opt/homebrew/include)
    if(EXISTS "${_hint}/boost/version.hpp")
        list(APPEND OPENPPP2_TEST_INCLUDES "${_hint}")
        break()
    endif()
endforeach()

enable_testing()

function(openppp2_add_cpp_test target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_executable(${target} ${ARG_SOURCES})
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE ${OPENPPP2_TEST_INCLUDES})
    target_compile_options(${target} PRIVATE
        -Wno-format
        -Wno-deprecated-declarations
        -Wno-tautological-constant-out-of-range-compare
    )
    target_compile_definitions(${target} PRIVATE FUNCTION)
    if(ENABLE_COVERAGE)
        target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(${target} PRIVATE -fprofile-instr-generate)
    endif()
    if(target STREQUAL "p2p_replay_window_test" OR target STREQUAL "base64_test")
        target_link_libraries(${target} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

openppp2_add_cpp_test(p2p_replay_window_test SOURCES p2p_replay_window_test.cpp)
openppp2_add_cpp_test(dns_buffer_test SOURCES dns_buffer_test.cpp "${OPENPPP2_ROOT}/common/dnslib/buffer.cpp")
openppp2_add_cpp_test(base64_test SOURCES base64_test.cpp "${OPENPPP2_ROOT}/common/base64/base64.cpp")
