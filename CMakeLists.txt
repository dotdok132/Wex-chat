cmake_minimum_required(VERSION 3.16)
project(wex_chat C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strict warnings and AddressSanitizer/UBsanitizer in Debug mode
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wstrict-prototypes)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

include_directories(
    include
    vendor/monocypher
)

# Core library
set(WEX_CORE_SOURCES
    vendor/monocypher/monocypher.c
    src/crypto/wex_crypto.c
)

add_library(wex_core STATIC ${WEX_CORE_SOURCES})

# Daemon executable
add_executable(wex_daemon src/main.c)
target_link_libraries(wex_daemon wex_core)

# Automated unit tests
enable_testing()

add_executable(test_crypto tests/test_crypto.c)
target_link_libraries(test_crypto wex_core)
add_test(NAME test_crypto COMMAND test_crypto)
