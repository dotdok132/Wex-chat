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
    vendor/cjson
)

# Core library sources
set(WEX_CORE_SOURCES
    vendor/monocypher/monocypher.c
    vendor/monocypher/monocypher-ed25519.c
    vendor/cjson/cJSON.c
    src/crypto/wex_crypto.c
    src/models/wex_models.c
    src/net/wex_ws_server.c
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

add_executable(test_models tests/test_models.c)
target_link_libraries(test_models wex_core)
add_test(NAME test_models COMMAND test_models)
