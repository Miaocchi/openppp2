cmake_minimum_required(VERSION 3.5.0)

project(openppp2_ohos LANGUAGES C CXX)

set(PROJECT_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..")
set(THIRD_PARTY_LIBRARY_DIR "$ENV{PPP_OHOS_THIRD_PARTY}" CACHE PATH "OHOS third-party library root")
if("${THIRD_PARTY_LIBRARY_DIR}" STREQUAL "")
    set(THIRD_PARTY_LIBRARY_DIR "/root/ohos")
endif()
if(NOT DEFINED OHOS_ARCH OR "${OHOS_ARCH}" STREQUAL "")
    if(DEFINED CMAKE_OHOS_ARCH_ABI AND NOT "${CMAKE_OHOS_ARCH_ABI}" STREQUAL "")
        set(OHOS_ARCH "${CMAKE_OHOS_ARCH_ABI}")
    else()
        set(OHOS_ARCH "arm64-v8a")
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -fvisibility=hidden -Wno-format -Wno-implicit-function-declaration -O3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -fvisibility=hidden -Wno-format -Wno-inconsistent-missing-override -Wno-pointer-bool-conversion -Wno-unsequenced -Wno-null-dereference -Wno-gnu-variable-sized-type-not-at-end -Wno-tautological-undefined-compare -Wno-deprecated-declarations -Wno-shift-count-overflow -O3")

add_definitions(-D_HARMONYOS)
add_definitions(-D_LINUX)
add_definitions(-D__MUSL__)
add_definitions(-DFUNCTION)
add_definitions(-DBUDDY_ALLOC_IMPLEMENTATION)

include_directories(
    ${PROJECT_ROOT_DIR}
    ${PROJECT_ROOT_DIR}/common
    ${PROJECT_ROOT_DIR}/common/json/include
    ${PROJECT_ROOT_DIR}/common/lwip/my
    ${PROJECT_ROOT_DIR}/common/lwip/include
    ${THIRD_PARTY_LIBRARY_DIR}/boost
    ${THIRD_PARTY_LIBRARY_DIR}/openssl/${OHOS_ARCH}/include
)

link_directories(
    ${THIRD_PARTY_LIBRARY_DIR}/boost/${OHOS_ARCH}
    ${THIRD_PARTY_LIBRARY_DIR}/openssl/${OHOS_ARCH}/lib
)

file(GLOB_RECURSE SOURCE_FILES
    ${PROJECT_ROOT_DIR}/common/lwip/my/*.c
    ${PROJECT_ROOT_DIR}/common/lwip/core/*.c
    ${PROJECT_ROOT_DIR}/common/lwip/api/*.c
    ${PROJECT_ROOT_DIR}/common/aggligator/*.cpp
    ${PROJECT_ROOT_DIR}/common/base64/*.cpp
    ${PROJECT_ROOT_DIR}/common/chnroutes2/*.cpp
    ${PROJECT_ROOT_DIR}/common/json/src/*.cpp
    ${PROJECT_ROOT_DIR}/common/libtcpip/*.cpp
    ${PROJECT_ROOT_DIR}/common/dnslib/*.cpp
    ${PROJECT_ROOT_DIR}/common/unix/*.c
    ${PROJECT_ROOT_DIR}/common/unix/*.cpp
    ${PROJECT_ROOT_DIR}/common/aesni/aes.cpp
    ${PROJECT_ROOT_DIR}/ppp/*.c
    ${PROJECT_ROOT_DIR}/ppp/*.cpp
    ${PROJECT_ROOT_DIR}/linux/*.c
    ${PROJECT_ROOT_DIR}/linux/*.cpp
)

add_library(openppp2 SHARED
    openppp2_ohos.cpp
    ${SOURCE_FILES}
)

target_link_libraries(openppp2
    libssl.a
    libcrypto.a
    libboost_system.a
    libboost_coroutine.a
    libboost_thread.a
    libboost_context.a
    libboost_regex.a
    libboost_filesystem.a
    libace_napi.z.so
    dl
)
