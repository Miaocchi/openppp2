CMAKE_MINIMUM_REQUIRED(VERSION 3.5) 

# Define the solutions name.
SET(NAME ppp)
PROJECT(${NAME} VERSION 2.1.10.0 LANGUAGES C CXX)# CMAKE_CURRENT_SOURCE_DIR

# C/CXX compiler configurations.
IF(MSVC)
    SET(CMAKE_C_FLAGS "")
    SET(CMAKE_CXX_FLAGS "")
    SET(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /nologo")
    ADD_COMPILE_OPTIONS(/utf-8)
    ADD_COMPILE_OPTIONS(/EHsc)
    ADD_COMPILE_OPTIONS(/MP)
    ADD_COMPILE_OPTIONS(/bigobj)
    ADD_COMPILE_OPTIONS(/wd4244)
    ADD_COMPILE_OPTIONS(/wd4819)
    ADD_COMPILE_OPTIONS(/wd4566)
    ADD_COMPILE_OPTIONS(/wd4309)
    ADD_COMPILE_OPTIONS(/wd4334)
    ADD_COMPILE_OPTIONS(/wd4267)
    ADD_DEFINITIONS(-DNOMINMAX)
    ADD_DEFINITIONS(-D_WIN32_WINNT=0x0601)
    ADD_DEFINITIONS(-D_CONSOLE)
    ADD_DEFINITIONS(-DUNICODE)
    ADD_DEFINITIONS(-D_UNICODE)
    ADD_DEFINITIONS(-D_CRT_SECURE_NO_WARNINGS)
    ADD_DEFINITIONS(-D_CRT_SECURE_NO_DEPRECATE)
    ADD_DEFINITIONS(-D_CRT_NONSTDC_NO_DEPRECATE)
    ADD_DEFINITIONS(-D_WINSOCK_DEPRECATED_NO_WARNINGS)
    ADD_DEFINITIONS(-D_CRT_NON_CONFORMING_SWPRINTFS)
    ADD_DEFINITIONS(-D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    ADD_DEFINITIONS(-D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
    ADD_DEFINITIONS(-D_SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS)
    ADD_DEFINITIONS(-DJEMALLOC)
    ADD_DEFINITIONS(-DFUNCTION)
    ADD_DEFINITIONS(-DBUDDY_ALLOC_IMPLEMENTATION)
    IF(CMAKE_SIZEOF_VOID_P EQUAL 4)
        ADD_DEFINITIONS(-DWIN32)
    ENDIF()
    STRING(REGEX REPLACE "([/-])M[TD]d?" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    STRING(REGEX REPLACE "([/-])M[TD]d?" "" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    STRING(REGEX REPLACE "([/-])M[TD]d?" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    STRING(REGEX REPLACE "([/-])M[TD]d?" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
    SET(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
    SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
ELSE()
    SET(CMAKE_C_FLAGS "-fPIE -fvisibility=hidden -Wno-format -Wno-implicit-function-declaration")
    SET(CMAKE_CXX_FLAGS "-fPIE -fvisibility=hidden -Wno-format -Wno-inconsistent-missing-override -Wno-pointer-bool-conversion -Wno-unsequenced -Wno-null-dereference -Wno-gnu-variable-sized-type-not-at-end -Wno-tautological-undefined-compare -Wno-deprecated-declarations -Wno-shift-count-overflow")
ENDIF()

# Configure predefined compilation macros.
# ADD_DEFINITIONS(-DANCIL_FD_BUFFER_STRUCT)
# ADD_DEFINITIONS(-DCURLINC_CURL)
# ADD_DEFINITIONS(-DTRANSMISSION_O1)
SET(ENABLE_SYSNAT_DEFAULT OFF)
IF(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    SET(ENABLE_SYSNAT_DEFAULT ON)
ENDIF()
OPTION(ENABLE_SYSNAT "Enable SYSNAT kernel-bypass traffic control (Linux only)" ${ENABLE_SYSNAT_DEFAULT})
IF(ENABLE_SYSNAT)
    ADD_DEFINITIONS(-DSYSNAT)
ENDIF()

# Diagnostic-only AddressSanitizer build (default OFF). When enabled on a
# non-MSVC toolchain it (1) drops jemalloc so ASan's allocator interceptors are
# actually in effect (jemalloc would otherwise take over malloc/free and hide
# heap errors), and (2) relaxes the fully-static link below so libasan can be
# linked. NEVER ship an ASan build to production; it is for reproducing the
# vmux teardown heap-corruption only.
OPTION(ENABLE_ASAN "Build with AddressSanitizer (diagnostic only; disables jemalloc)" OFF)
OPTION(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer (diagnostic only)" OFF)
OPTION(ENABLE_TSAN "Build with ThreadSanitizer (Linux diagnostic only; disables jemalloc)" OFF)
IF(ENABLE_TSAN AND (ENABLE_ASAN OR ENABLE_UBSAN))
    MESSAGE(FATAL_ERROR "ENABLE_TSAN must use a separate build from ASan/UBSan")
ENDIF()
IF(ENABLE_TSAN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    MESSAGE(FATAL_ERROR "ENABLE_TSAN is supported only for Linux diagnostic builds")
ENDIF()
SET(PPP_SANITIZER_FLAGS "")
IF(ENABLE_ASAN)
    SET(PPP_SANITIZER_FLAGS "${PPP_SANITIZER_FLAGS} -fsanitize=address")
ENDIF()
IF(ENABLE_UBSAN)
    SET(PPP_SANITIZER_FLAGS "${PPP_SANITIZER_FLAGS} -fsanitize=undefined")
ENDIF()
IF(ENABLE_TSAN)
    SET(PPP_SANITIZER_FLAGS "${PPP_SANITIZER_FLAGS} -fsanitize=thread")
    ADD_DEFINITIONS(-DPPP_COROUTINES_TSAN_ENABLED=1)
ENDIF()
IF((ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN) AND NOT MSVC)
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${PPP_SANITIZER_FLAGS} -fno-omit-frame-pointer -g")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${PPP_SANITIZER_FLAGS} -fno-omit-frame-pointer -g")
ENDIF()
IF(NOT ENABLE_ASAN AND NOT ENABLE_UBSAN AND NOT ENABLE_TSAN)
    ADD_DEFINITIONS(-DJEMALLOC)
ENDIF()
ADD_DEFINITIONS(-DFUNCTION)
ADD_DEFINITIONS(-DBUDDY_ALLOC_IMPLEMENTATION)

# This file is shared with liburing and that has to autodetect
# If linux/time_types.h is available or not, it can
# Define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H
# If linux/time_types.h is not available
# ADD_DEFINITIONS(-DUAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H)

# When compiling with the musl-libc standard library, 
# You need to define the _MUSL__ preprocessor macro to ensure correct compilation.
# https://wiki.musl-libc.org/faq
# ADD_DEFINITIONS(-D__MUSL__)

# When using the musl-libc standard library, the _LARGEFILE64_SOURCE macro is not defined by default on some platforms. 
# If 64-bit functions extended by _LARGEFILE64_SOURCE, such as lseek64, are required, it needs to be explicitly defined.
# ADD_DEFINITIONS(-D_LARGEFILE64_SOURCE)

# ENV.
SET(PLATFORM_DEBUG FALSE)
IF(CMAKE_BUILD_TYPE STREQUAL "Debug" OR BT MATCHES "Debug")
    SET(PLATFORM_DEBUG TRUE)
ENDIF()

SET(PLATFORM_COMPILER_CLANG FALSE)
SET(PLATFORM_SYSTEM_LINUX FALSE)
SET(PLATFORM_SYSTEM_DARWIN FALSE)
SET(PLATFORM_SYSTEM_WINDOWS FALSE)

IF(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    SET(PLATFORM_COMPILER_CLANG TRUE)
ENDIF()

IF(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME MATCHES "Windows")
    SET(PLATFORM_SYSTEM_WINDOWS TRUE)
ELSEIF(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "Darwin")
    SET(PLATFORM_SYSTEM_DARWIN TRUE)
ELSE()
    SET(PLATFORM_SYSTEM_LINUX TRUE)
ENDIF()

IF(PLATFORM_SYSTEM_WINDOWS)
    IF(NOT CMAKE_BUILD_TYPE)
        SET(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    ENDIF()

    IF(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        MESSAGE(FATAL_ERROR "Windows builds only support Debug or Release.")
    ENDIF()
ENDIF()

# C
IF(PLATFORM_DEBUG)
    IF(MSVC)
        ADD_DEFINITIONS(-D_DEBUG)
    ELSE()
        SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_DEBUG -g3")
    ENDIF()
ELSEIF(NOT MSVC)
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
ENDIF()

# C/CXX
IF(PLATFORM_DEBUG)
    IF(MSVC)
        ADD_DEFINITIONS(-D_DEBUG)
    ELSE()
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DEBUG -g3")
    ENDIF()
ELSEIF(NOT MSVC)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
ENDIF()

IF(MSVC AND NOT PLATFORM_DEBUG)
    ADD_DEFINITIONS(-DNDEBUG)
ENDIF()

# Reduce the display of compiler warnings.
IF(NOT MSVC)
    ADD_COMPILE_OPTIONS(-Wno-deprecated-declarations)
ENDIF()

# Linux production builds use io_uring by default. Compatibility builds for
# kernels before 5.10 must explicitly pass -DENABLE_IO_URING=OFF.
SET(ENABLE_IO_URING_DEFAULT OFF)
IF(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    SET(ENABLE_IO_URING_DEFAULT ON)
ENDIF()
OPTION(ENABLE_IO_URING "Enable io_uring async event driver for Boost.Asio (Linux kernel >= 5.10)" ${ENABLE_IO_URING_DEFAULT})
IF(ENABLE_IO_URING AND PLATFORM_SYSTEM_LINUX)
    ADD_DEFINITIONS(-DBOOST_ASIO_HAS_IO_URING=1)
    ADD_DEFINITIONS(-DBOOST_ASIO_DISABLE_EPOLL=1)
ENDIF()

# Check C/CXX compiler supports.
INCLUDE(CheckCXXCompilerFlag)
IF(MSVC)
    SET(COMPILER_SUPPORTS_CXX17 TRUE)
    SET(COMPILER_SUPPORTS_CXX1Z FALSE)
ELSE()
    CHECK_CXX_COMPILER_FLAG("-std=c++17" COMPILER_SUPPORTS_CXX17)
    CHECK_CXX_COMPILER_FLAG("-std=c++1z" COMPILER_SUPPORTS_CXX1Z)
ENDIF()

# Generic builds must not assume AES-NI, AVX, or AVX2.  The optimized
# production profiles opt in explicitly (see the Linux package workflows).
SET(ENABLE_SIMD_DEFAULT OFF)
OPTION(ENABLE_SIMD "Enable x86/x64 SIMD AES-NI implementation" ${ENABLE_SIMD_DEFAULT})

# SSEA's dispatchable implementations are compiled only when the SIMD profile
# is selected.  Their instruction flags are applied to the two translation
# units that own the runtime dispatch; they must never leak into the rest of
# the executable.
OPTION(ENABLE_SSEA_SIMD "Enable SSSE3+AVX2 SSEA SIMD acceleration (requires ENABLE_SIMD)" ${ENABLE_SIMD_DEFAULT})

OPTION(ENABLE_TESTS "Build unit tests (GTest)" OFF)
OPTION(ENABLE_XTCP_WINDOWS_TESTS "Build the offline Windows XTCP lifecycle test" OFF)
INCLUDE(cmake/Corocc.cmake)
OPTION(ENABLE_P2P_RECOVERY_TESTS "Build isolated actual Exchanger recovery tests (test-only seams)" OFF)
OPTION(ENABLE_P2P_V2_PRODUCTION "Enable authenticated P2P v2 production capability" ON)
OPTION(ENABLE_VMUX_CHURN_TEST "Build the root-linked VMUX carrier churn test" OFF)
OPTION(ENABLE_VMUX_RECEIVE_SEMANTICS_TEST "Build the root-linked VMUX receive-semantics test" OFF)
OPTION(ENABLE_COVERAGE "Enable LLVM source coverage instrumentation" OFF)
SET(ENABLE_XTCP_DEFAULT OFF)
IF(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    SET(ENABLE_XTCP_DEFAULT ON)
ELSEIF(ANDROID)
    SET(ENABLE_XTCP_DEFAULT ON)
ELSEIF(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8
    AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|arm|aarch"
    AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "ARM|arm")
    SET(ENABLE_XTCP_DEFAULT ON)
ENDIF()
# XTCP is compiled into every supported production target. Runtime selection
# remains explicit (--tcp-stack=xtcp); unsupported Windows ARM/x86 targets
# retain the native/lwIP-only build.
OPTION(ENABLE_XTCP "Build with the pinned XTCP userspace TCP/IP stack" ${ENABLE_XTCP_DEFAULT})
SET(XTCP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/xtcp" CACHE PATH "Path to the pinned XTCP source tree")
IF(ENABLE_COVERAGE AND NOT MSVC)
    INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Coverage.cmake)
ENDIF()

IF(ENABLE_XTCP)
    IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32 AND NOT ANDROID)
        MESSAGE(FATAL_ERROR "ENABLE_XTCP is supported only for Linux, Windows, and Android builds.")
    ENDIF()
    IF(WIN32 AND (NOT CMAKE_SIZEOF_VOID_P EQUAL 8
        OR CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM"
        OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$"))
        MESSAGE(FATAL_ERROR "Windows XTCP currently supports x64 only.")
    ENDIF()
    IF(NOT EXISTS "${XTCP_SOURCE_DIR}/CMakeLists.txt")
        MESSAGE(FATAL_ERROR "XTCP source tree not found at ${XTCP_SOURCE_DIR}; run tools/prepare_xtcp.sh first.")
    ENDIF()

    SET(XTCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    SET(XTCP_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    SET(XTCP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    SET(XTCP_BUILD_PLUGINS OFF CACHE BOOL "" FORCE)
    SET(XTCP_BUILD_LWIP OFF CACHE BOOL "" FORCE)
    SET(XTCP_BUILD_CC_PLUGINS ON CACHE BOOL "" FORCE)
    SET(XTCP_CHECKSUM_VALIDATE ON CACHE BOOL "" FORCE)
    ADD_SUBDIRECTORY("${XTCP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/third-party/xtcp" EXCLUDE_FROM_ALL)
    IF(NOT TARGET xtcp_static)
        MESSAGE(FATAL_ERROR "XTCP source tree did not define the required xtcp_static target.")
    ENDIF()
ENDIF()

SET(PLATFORM_ARCH_X86 FALSE)
SET(PLATFORM_ARCH_ARM64 FALSE)
IF(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64|i[3-6]86|x86)$")
    SET(PLATFORM_ARCH_X86 TRUE)
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|AARCH64|arm64|ARM64)$")
    SET(PLATFORM_ARCH_ARM64 TRUE)
ELSEIF(MSVC AND (CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SIZEOF_VOID_P EQUAL 4))
    # MSVC cross-compiling: check _M_ARM64 at compile time
    # For now, assume x86/x64 for MSVC unless explicitly targeting ARM64
    SET(PLATFORM_ARCH_X86 TRUE)
ENDIF()

# CMake's MSVC host pointer size describes the compiler host, not the selected
# target platform. Correct the architecture flags for ARM64 generators so an
# explicit ENABLE_SIMD=OFF remains effective and ARM64 never receives x86 AES.
IF(MSVC AND (CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|arm64)$"
    OR CMAKE_VS_PLATFORM_NAME MATCHES "^(ARM64|arm64)$"))
    SET(PLATFORM_ARCH_X86 FALSE)
    SET(PLATFORM_ARCH_ARM64 TRUE)
ENDIF()

# For MSVC ARM64 cross-compilation, detect at compile time
IF(MSVC AND NOT PLATFORM_ARCH_X86 AND NOT PLATFORM_ARCH_ARM64)
    # This will be handled by _M_ARM64 in source code
ENDIF()

SET(__SIMD__ FALSE)
IF(PLATFORM_SYSTEM_WINDOWS AND PLATFORM_ARCH_X86 AND ENABLE_SIMD)
    SET(__SIMD__ TRUE)
ENDIF()

IF(ENABLE_SIMD)
    IF(PLATFORM_ARCH_X86)
        SET(__SIMD__ TRUE)
    ELSE()
        MESSAGE(WARNING "ENABLE_SIMD was requested on a non-x86/x64 target; SIMD AES-NI will remain disabled.")
    ENDIF()
ENDIF()

IF(__SIMD__)
    # Keep ISA flags off the target-wide compiler command line.  AES and SSEA
    # contain runtime CPUID dispatch, so only their owning translation units
    # may be compiled with the corresponding instruction sets.
    IF(MSVC)
        SET(SIMD_BASE_COMPILE_OPTIONS)
        IF(CMAKE_SIZEOF_VOID_P EQUAL 4)
            SET(SIMD_BASE_COMPILE_OPTIONS /arch:SSE2)
        ENDIF()
        SET(AES_COMPILE_OPTIONS ${SIMD_BASE_COMPILE_OPTIONS})
        SET(SSEA_COMPILE_OPTIONS ${SIMD_BASE_COMPILE_OPTIONS})
    ELSE()
        SET(SIMD_BASE_COMPILE_OPTIONS -msse2)
        SET(AES_COMPILE_OPTIONS -maes -mpclmul ${SIMD_BASE_COMPILE_OPTIONS})
        SET(SSEA_COMPILE_OPTIONS ${SIMD_BASE_COMPILE_OPTIONS})
    ENDIF()

    IF(NOT MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8.0)
        LIST(APPEND AES_COMPILE_OPTIONS -march=westmere -fno-stack-protector)
        LIST(APPEND SSEA_COMPILE_OPTIONS -march=westmere -fno-stack-protector)
    ENDIF()

    IF(ENABLE_SSEA_SIMD)
        IF(NOT MSVC AND PLATFORM_ARCH_X86)
            LIST(APPEND SSEA_COMPILE_OPTIONS
                -mssse3 -msse4.1 -msse4.2 -mpopcnt -mavx -mavx2)
        ENDIF()
    ENDIF()

    ADD_DEFINITIONS(-D__SIMD__)

    FILE(GLOB_RECURSE AES_NI_SOURCE_FILES
        ${PROJECT_SOURCE_DIR}/common/aesni/impl/*.c
        ${PROJECT_SOURCE_DIR}/common/aesni/impl/*.cpp)
    LIST(APPEND AES_NI_SOURCE_FILES
        ${PROJECT_SOURCE_DIR}/common/aesni/aes.cpp)

    SET_SOURCE_FILES_PROPERTIES(${AES_NI_SOURCE_FILES}
        PROPERTIES COMPILE_OPTIONS "${AES_COMPILE_OPTIONS}")
    SET_SOURCE_FILES_PROPERTIES(
        ${PROJECT_SOURCE_DIR}/ppp/net/native/checksum.cpp
        PROPERTIES COMPILE_OPTIONS "${SIMD_BASE_COMPILE_OPTIONS}")
    SET_SOURCE_FILES_PROPERTIES(
        ${PROJECT_SOURCE_DIR}/ppp/cryptography/ssea.cpp
        ${PROJECT_SOURCE_DIR}/ppp/transmissions/ITransmission.cpp
        PROPERTIES COMPILE_OPTIONS "${SSEA_COMPILE_OPTIONS}")
ENDIF()

# According to the type of operating system to determine whether you need to check whether the compiler supports C/C++17, 
# On the MACOS system, the following check code is invalid, so directly specify the standard version of C/C++, 
# If an error or compilation does not pass, it means that you need to upgrade the LLVM GCC/CLANG compiler installed on MACOS.
IF(PLATFORM_SYSTEM_DARWIN)
    # The mandatory version is C++17.
    SET(CMAKE_CXX_STANDARD 17)

    # Minimum macOS deployment target for backward compatibility (macOS 13 Ventura).
    IF(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        SET(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment target")
    ENDIF()

    ADD_DEFINITIONS(-D_GNU_SOURCE)
    ADD_DEFINITIONS(-DBOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED)
ELSE()
    # CXX17
    IF(MSVC)
        SET(CMAKE_CXX_STANDARD 17)
    ELSEIF(COMPILER_SUPPORTS_CXX17)
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
    ELSEIF(COMPILER_SUPPORTS_CXX1Z)
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++1z")
    ELSE()
        MESSAGE(STATUS "The compiler ${CMAKE_CXX_COMPILER} has no C++ 17 support. Please use a different C++ compiler.")
    ENDIF()
    
    # When the __MUSL__ macro is enabled and compilation errors are encountered, you can add the connector option: -static
    # -rdynamic -Wl,-Bstatic -Wl,-Bdynamic -lstdc++ -lpthread -ldl -lz -lrt
    IF(NOT MSVC)
        IF(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN)
            # Sanitizers need their runtimes dynamically linked; do not force -Bstatic.
            SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -rdynamic ${PPP_SANITIZER_FLAGS}")
        ELSE()
            SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -rdynamic -Wl,-Bstatic") 
        ENDIF()
    ENDIF()
ENDIF()

# Set the tripartite library directorys.
SET(THIRD_PARTY_LIBRARY_DIR /root/dev CACHE PATH "Third-party library root directory")

IF(PLATFORM_SYSTEM_WINDOWS)
    SET(OPENSSL_USE_STATIC_LIBS TRUE)

    IF(NOT VCPKG_TARGET_TRIPLET)
        MESSAGE(FATAL_ERROR "VCPKG_TARGET_TRIPLET is required on Windows.")
    ENDIF()

    IF(VCPKG_INSTALLED_DIR)
        SET(VCPKG_DEP_DIR ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET})
    ELSEIF(VCPKG_ROOT_DIR)
        SET(VCPKG_DEP_DIR ${VCPKG_ROOT_DIR}/installed/${VCPKG_TARGET_TRIPLET})
    ELSEIF(VCPKG_ROOT)
        SET(VCPKG_DEP_DIR ${VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET})
    ENDIF()

    SET(Boost_NO_SYSTEM_PATHS ON)
    SET(Boost_USE_STATIC_LIBS ON)
    SET(Boost_NO_WARN_NEW_VERSIONS ON)
    SET(BOOST_ROOT ${VCPKG_DEP_DIR})
    SET(BOOST_INCLUDEDIR ${VCPKG_DEP_DIR}/include)
    SET(BOOST_LIBRARYDIR ${VCPKG_DEP_DIR}/lib)
    SET(OPENSSL_ROOT_DIR ${VCPKG_DEP_DIR})

    FIND_PACKAGE(Boost REQUIRED COMPONENTS system filesystem regex thread coroutine context)
    FIND_PACKAGE(OpenSSL REQUIRED)
    FIND_PATH(JEMALLOC_INCLUDE_DIR NAMES jemalloc/jemalloc.h HINTS ${VCPKG_DEP_DIR}/include NO_DEFAULT_PATH)
    FIND_LIBRARY(JEMALLOC_LIBRARY NAMES jemalloc_s jemalloc libjemalloc HINTS ${VCPKG_DEP_DIR}/lib ${VCPKG_DEP_DIR}/debug/lib NO_DEFAULT_PATH)

    IF(NOT JEMALLOC_INCLUDE_DIR OR NOT JEMALLOC_LIBRARY)
        MESSAGE(FATAL_ERROR "Could not locate jemalloc from the active vcpkg installation for ${VCPKG_TARGET_TRIPLET}.")
    ENDIF()
ENDIF()

# Set the compiled header file search directorys.
INCLUDE_DIRECTORIES(
    ${PROJECT_SOURCE_DIR}/
    ${PROJECT_SOURCE_DIR}/common
    ${PROJECT_SOURCE_DIR}/common/json/include
    ${PROJECT_SOURCE_DIR}/common/lwip/my
    ${PROJECT_SOURCE_DIR}/common/lwip/include
)

IF(NOT PLATFORM_SYSTEM_WINDOWS)
    INCLUDE_DIRECTORIES(
        ${THIRD_PARTY_LIBRARY_DIR}/boost
        ${THIRD_PARTY_LIBRARY_DIR}/jemalloc/include
        ${THIRD_PARTY_LIBRARY_DIR}/openssl/include
    )
ENDIF()

# Set the third library connection directory to searchs.
IF(NOT PLATFORM_SYSTEM_WINDOWS)
    LINK_DIRECTORIES(
        ${THIRD_PARTY_LIBRARY_DIR}/boost/stage/lib
        ${THIRD_PARTY_LIBRARY_DIR}/jemalloc/lib
        ${THIRD_PARTY_LIBRARY_DIR}/openssl

        # ${THIRD_PARTY_LIBRARY_DIR}/liburing/src
        # ${THIRD_PARTY_LIBRARY_DIR}/curl/lib/.libs
    )
ENDIF()

# Set the compiled source file search directorys.
IF(MSVC)
    FILE(GLOB_RECURSE PLATFORM_SOURCE_FILES 
        ${PROJECT_SOURCE_DIR}/windows/*.c 
        ${PROJECT_SOURCE_DIR}/windows/*.cpp)
ELSEIF(PLATFORM_SYSTEM_DARWIN)
    FILE(GLOB_RECURSE PLATFORM_SOURCE_FILES 
        ${PROJECT_SOURCE_DIR}/darwin/*.c 
        ${PROJECT_SOURCE_DIR}/darwin/*.cpp)
ELSE()
    FILE(GLOB_RECURSE PLATFORM_SOURCE_FILES 
        ${PROJECT_SOURCE_DIR}/linux/*.c 
        ${PROJECT_SOURCE_DIR}/linux/*.cpp)
ENDIF()

# List of common cpp source files compilations for ppp/unix cmake solution projects.
#
# NOTE: Do NOT pass a bare filename such as "${PROJECT_SOURCE_DIR}/main.cpp" to
# FILE(GLOB_RECURSE ...). CMake interprets the last path component as a name
# pattern and walks the entire tree, which means any stray main.cpp sitting
# inside a vendored directory (e.g. third-party/boost/doc/html/boost_asio/
# example/**/main.cpp) gets silently added to this target. The Boost asio
# examples include C++20 coroutine code that cannot be built under -std=c++17
# and blew up the Linux CI build. main.cpp is added explicitly below.
FILE(GLOB_RECURSE SOURCE_FILES 
    ${PROJECT_SOURCE_DIR}/common/lwip/my/*.c 
    ${PROJECT_SOURCE_DIR}/common/lwip/core/*.c 
    ${PROJECT_SOURCE_DIR}/common/lwip/api/*.c 
    # ${PROJECT_SOURCE_DIR}/common/lwip/netif/*.c

    ${PROJECT_SOURCE_DIR}/common/aggligator/*.cpp 
    ${PROJECT_SOURCE_DIR}/common/base64/*.cpp 
    ${PROJECT_SOURCE_DIR}/common/chnroutes2/*.cpp 
    ${PROJECT_SOURCE_DIR}/common/json/src/*.cpp 
    ${PROJECT_SOURCE_DIR}/common/libtcpip/*.cpp 
    ${PROJECT_SOURCE_DIR}/common/dnslib/*.cpp 
    ${PROJECT_SOURCE_DIR}/common/aesni/aes.cpp

    ${PROJECT_SOURCE_DIR}/ppp/*.c 
    ${PROJECT_SOURCE_DIR}/ppp/*.cpp)

FILE(GLOB PPP_P2P_SOURCE_FILES ${PROJECT_SOURCE_DIR}/ppp/p2p/*.cpp)
LIST(APPEND SOURCE_FILES ${PPP_P2P_SOURCE_FILES})

LIST(APPEND SOURCE_FILES ${PROJECT_SOURCE_DIR}/main.cpp)

IF(PLATFORM_SYSTEM_WINDOWS)
    LIST(APPEND SOURCE_FILES ${PROJECT_SOURCE_DIR}/resource.rc)
ENDIF()

IF(__SIMD__)
    LIST(APPEND SOURCE_FILES ${AES_NI_SOURCE_FILES})
ENDIF()

IF(NOT MSVC)
    FILE(GLOB_RECURSE UNIX_SOURCE_FILES 
        ${PROJECT_SOURCE_DIR}/common/unix/*.c 
        ${PROJECT_SOURCE_DIR}/common/unix/*.cpp)
    LIST(APPEND SOURCE_FILES ${UNIX_SOURCE_FILES})
ENDIF()

# openppp2_sysnat.c requires libbpf headers; only compile it when SYSNAT is enabled.
LIST(REMOVE_ITEM PLATFORM_SOURCE_FILES
    ${PROJECT_SOURCE_DIR}/linux/ppp/tap/openppp2_sysnat.c)
IF(ENABLE_SYSNAT)
    LIST(APPEND PLATFORM_SOURCE_FILES
        ${PROJECT_SOURCE_DIR}/linux/ppp/tap/openppp2_sysnat.c)
ENDIF()

SET(PPP_MAIN_SOURCE ${PROJECT_SOURCE_DIR}/main.cpp)
SET(PPP_LIBRARY_SOURCES ${SOURCE_FILES} ${PLATFORM_SOURCE_FILES})
LIST(REMOVE_ITEM PPP_LIBRARY_SOURCES ${PPP_MAIN_SOURCE})
IF(PLATFORM_SYSTEM_WINDOWS)
    LIST(REMOVE_ITEM PPP_LIBRARY_SOURCES ${PROJECT_SOURCE_DIR}/resource.rc)
ENDIF()

ADD_LIBRARY(openppp2_lib STATIC ${PPP_LIBRARY_SOURCES})
IF(ENABLE_P2P_V2_PRODUCTION)
    TARGET_COMPILE_DEFINITIONS(openppp2_lib PUBLIC OPENPPP2_P2P_V2_PRODUCTION=1)
ELSE()
    TARGET_COMPILE_DEFINITIONS(openppp2_lib PUBLIC OPENPPP2_P2P_V2_PRODUCTION=0)
ENDIF()
IF(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9
    AND NOT ANDROID)
    TARGET_LINK_LIBRARIES(openppp2_lib stdc++fs)
ENDIF()
IF(ENABLE_P2P_RECOVERY_TESTS)
    TARGET_COMPILE_DEFINITIONS(openppp2_lib PUBLIC OPENPPP2_P2P_RECOVERY_TESTING=1)
ENDIF()
IF(ENABLE_COROCC)
    TARGET_LINK_LIBRARIES(openppp2_lib ppp_context_backend)
ENDIF()
IF(ENABLE_XTCP)
    TARGET_LINK_LIBRARIES(openppp2_lib xtcp_static)
    IF(WIN32)
        # The pinned vendor target only declares these for MinGW. MSVC/Ninja
        # must link the same system libraries as the opt-in vcxproj build.
        TARGET_LINK_LIBRARIES(openppp2_lib bcrypt winmm)
    ENDIF()
    TARGET_COMPILE_DEFINITIONS(openppp2_lib PRIVATE
        PPP_ENABLE_XTCP=1
        PPP_XTCP_RUNTIME_WIRED=1
        PPP_XTCP_HAS_TIMER_DEADLINE=1)
ENDIF()

SET(PPP_EXECUTABLE_SOURCES ${PPP_MAIN_SOURCE})
IF(PLATFORM_SYSTEM_WINDOWS)
    LIST(APPEND PPP_EXECUTABLE_SOURCES ${PROJECT_SOURCE_DIR}/resource.rc)
ENDIF()

ADD_EXECUTABLE(${NAME} ${PPP_EXECUTABLE_SOURCES})

# Set the compilation output files path.
IF(NOT PPP_OUTPUT_DIR)
    IF(PLATFORM_SYSTEM_WINDOWS)
        SET(PPP_OUTPUT_DIR ${PROJECT_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE})
    ELSE()
        SET(PPP_OUTPUT_DIR ${PROJECT_SOURCE_DIR}/bin)
    ENDIF()
ENDIF()
SET(EXECUTABLE_OUTPUT_PATH ${PPP_OUTPUT_DIR})

# Set up library connections to dependent libraries.
IF(PLATFORM_SYSTEM_WINDOWS)
    INCLUDE_DIRECTORIES(
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
        ${JEMALLOC_INCLUDE_DIR})

    SET_TARGET_PROPERTIES(${NAME} PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")

    # PPP stays an executable; only the third-party dependencies are static.
    TARGET_LINK_LIBRARIES(openppp2_lib
        ws2_32
        iphlpapi
        setupapi
        ole32
        oleaut32
        wbemuuid
        advapi32
        userenv
        crypt32
        bcrypt
        ncrypt
        dnsapi

        ${Boost_LIBRARIES}
        ${OPENSSL_SSL_LIBRARY}
        ${OPENSSL_CRYPTO_LIBRARY}
        ${JEMALLOC_LIBRARY})

    TARGET_LINK_LIBRARIES(${NAME} openppp2_lib)
ELSEIF(PLATFORM_SYSTEM_DARWIN)
    SET(BUILD_SHARED_LIBS ON) # System

    # ASan diagnostic build drops jemalloc so the sanitizer's allocator
    # interceptors are in effect; default build links jemalloc as before.
    IF(ENABLE_ASAN OR ENABLE_UBSAN)
        SET(PPP_DARWIN_ALLOC_LIB "")
    ELSE()
        SET(PPP_DARWIN_ALLOC_LIB ${THIRD_PARTY_LIBRARY_DIR}/jemalloc/lib/libjemalloc.a)
    ENDIF()

    TARGET_LINK_LIBRARIES(openppp2_lib
        ${THIRD_PARTY_LIBRARY_DIR}/openssl/libssl.a 
        ${THIRD_PARTY_LIBRARY_DIR}/openssl/libcrypto.a 
        ${PPP_DARWIN_ALLOC_LIB}

        dl
        pthread

        ${THIRD_PARTY_LIBRARY_DIR}/boost/stage/lib/libboost_system.a
        ${THIRD_PARTY_LIBRARY_DIR}/boost/stage/lib/libboost_coroutine.a 
        ${THIRD_PARTY_LIBRARY_DIR}/boost/stage/lib/libboost_thread.a 
        ${THIRD_PARTY_LIBRARY_DIR}/boost/stage/lib/libboost_context.a 
        ${THIRD_PARTY_LIBRARY_DIR}/boost/stage/lib/libboost_regex.a 
        ${THIRD_PARTY_LIBRARY_DIR}/boost/stage/lib/libboost_filesystem.a
        "-framework Security"
        "-framework CoreFoundation")

    TARGET_LINK_LIBRARIES(${NAME} openppp2_lib)
ELSEIF(PLATFORM_COMPILER_CLANG)
    SET(BUILD_SHARED_LIBS ON)

    SET(PPP_CLANG_LINK_LIBS
        libssl.a
        libcrypto.a
        atomic
        dl
        pthread
        libboost_system.a
        libboost_coroutine.a
        libboost_thread.a
        libboost_context.a
        libboost_regex.a
        libboost_filesystem.a)
    IF(NOT ENABLE_ASAN AND NOT ENABLE_UBSAN AND NOT ENABLE_TSAN)
        LIST(APPEND PPP_CLANG_LINK_LIBS libjemalloc.a)
    ENDIF()
    LIST(APPEND PPP_CLANG_LINK_LIBS z zstd)
    IF(ENABLE_IO_URING)
        LIST(APPEND PPP_CLANG_LINK_LIBS uring)
    ENDIF()
    IF(ENABLE_SYSNAT)
        LIST(APPEND PPP_CLANG_LINK_LIBS bpf)
    ENDIF()
    TARGET_LINK_LIBRARIES(openppp2_lib ${PPP_CLANG_LINK_LIBS})
    TARGET_LINK_LIBRARIES(${NAME} openppp2_lib)
ELSEIF(NOT MSVC)
    IF(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN)
        # ASan variant: drop libc.a (no full-static libc) and libjemalloc.a (let
        # ASan's allocator intercept malloc/free). Diagnostic build only.
        SET(PPP_LINUX_LINK_LIBS
            libssl.a
            libcrypto.a
            m
            unwind
            atomic
            stdc++
            rt
            dl
            pthread
            libboost_system.a
            libboost_coroutine.a
            libboost_thread.a
            libboost_context.a
            libboost_regex.a
            libboost_filesystem.a)
    ELSE()
        SET(PPP_LINUX_LINK_LIBS
            libc.a
            libssl.a
            libcrypto.a
            libjemalloc.a
            m
            unwind
            atomic
            stdc++
            rt
            dl
            pthread
            libboost_system.a
            libboost_coroutine.a
            libboost_thread.a
            libboost_context.a
            libboost_regex.a
            libboost_filesystem.a)
    ENDIF()
    IF(ENABLE_IO_URING)
        LIST(APPEND PPP_LINUX_LINK_LIBS uring)
    ENDIF()
    IF(ENABLE_SYSNAT)
        LIST(APPEND PPP_LINUX_LINK_LIBS bpf)
    ENDIF()
    TARGET_LINK_LIBRARIES(openppp2_lib ${PPP_LINUX_LINK_LIBS} z zstd)
    TARGET_LINK_LIBRARIES(${NAME} openppp2_lib)
ENDIF()

IF(ENABLE_VMUX_CHURN_TEST)
    ENABLE_TESTING()
    ADD_EXECUTABLE(vmux_net_churn_integration_test
        tests/cpp/vmux_net_churn_integration_test.cpp)
    TARGET_LINK_LIBRARIES(vmux_net_churn_integration_test openppp2_lib)
    IF(PLATFORM_SYSTEM_LINUX)
        TARGET_LINK_OPTIONS(vmux_net_churn_integration_test PRIVATE
            "-Wl,--exclude-libs,ALL")
    ENDIF()
    ADD_TEST(NAME vmux_net_churn_integration_test COMMAND vmux_net_churn_integration_test)
ENDIF()


IF(ENABLE_VMUX_RECEIVE_SEMANTICS_TEST)
    ENABLE_TESTING()
    ADD_EXECUTABLE(vmux_receive_semantics_test
        tests/cpp/vmux_receive_semantics_test.cpp)
    TARGET_LINK_LIBRARIES(vmux_receive_semantics_test openppp2_lib)
    IF(PLATFORM_SYSTEM_LINUX)
        TARGET_LINK_OPTIONS(vmux_receive_semantics_test PRIVATE
            "-Wl,--exclude-libs,ALL")
    ENDIF()
    ADD_TEST(NAME vmux_receive_semantics_test COMMAND vmux_receive_semantics_test)
ENDIF()

IF(ENABLE_TESTS)
    ENABLE_TESTING()
    ADD_SUBDIRECTORY(tests)
ENDIF()

IF(ENABLE_P2P_RECOVERY_TESTS)
    ENABLE_TESTING()
    ADD_EXECUTABLE(p2p_v2_exchanger_recovery_test
        tests/cpp/p2p_v2_exchanger_recovery_test.cpp)
    TARGET_LINK_LIBRARIES(p2p_v2_exchanger_recovery_test openppp2_lib)
    ADD_TEST(NAME p2p_v2_exchanger_recovery_test COMMAND p2p_v2_exchanger_recovery_test)
    SET_TESTS_PROPERTIES(p2p_v2_exchanger_recovery_test PROPERTIES TIMEOUT 120)
ENDIF()

IF(ENABLE_XTCP_WINDOWS_TESTS)
    IF(NOT WIN32 OR NOT ENABLE_XTCP)
        MESSAGE(FATAL_ERROR "ENABLE_XTCP_WINDOWS_TESTS requires Windows and ENABLE_XTCP=ON.")
    ENDIF()
    ENABLE_TESTING()
    ADD_EXECUTABLE(xtcp_windows_lifecycle_test tests/cpp/xtcp_windows_lifecycle_test.cpp)
    TARGET_LINK_LIBRARIES(xtcp_windows_lifecycle_test openppp2_lib)
    ADD_TEST(NAME xtcp_windows_lifecycle_test COMMAND xtcp_windows_lifecycle_test)
    SET_TESTS_PROPERTIES(xtcp_windows_lifecycle_test PROPERTIES TIMEOUT 30)
    ADD_EXECUTABLE(xtcp_runtime_bridge_test tests/cpp/xtcp_runtime_bridge_test.cpp)
    TARGET_LINK_LIBRARIES(xtcp_runtime_bridge_test openppp2_lib)
    TARGET_INCLUDE_DIRECTORIES(xtcp_runtime_bridge_test PRIVATE "${XTCP_SOURCE_DIR}/tests")
    ADD_TEST(NAME xtcp_runtime_bridge_test COMMAND xtcp_runtime_bridge_test)
    SET_TESTS_PROPERTIES(xtcp_runtime_bridge_test PROPERTIES TIMEOUT 120)
ENDIF()
