include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

add_executable(openppp2_tests
    unit/test_application_mode.cpp
    unit/test_tap_stub.cpp
    unit/test_proxy_defaults.cpp
)

target_link_libraries(openppp2_tests
    PRIVATE
        openppp2_lib
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(openppp2_tests)
