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
    src/net/wex_p2p.c
    src/net/wex_nat.c
    src/storage/wex_store.c
    src/dht/wex_dht.c
)

add_library(wex_core STATIC ${WEX_CORE_SOURCES})

# Daemon executable
add_executable(wex_daemon src/main.c)
target_link_libraries(wex_daemon wex_core)

# Automated unit & integration tests
enable_testing()

add_executable(test_crypto tests/test_crypto.c)
target_link_libraries(test_crypto wex_core)
add_test(NAME test_crypto COMMAND test_crypto)

add_executable(test_models tests/test_models.c)
target_link_libraries(test_models wex_core)
add_test(NAME test_models COMMAND test_models)

add_executable(test_p2p_swarm tests/test_p2p_swarm.c)
target_link_libraries(test_p2p_swarm wex_core)
add_test(NAME test_p2p_swarm COMMAND test_p2p_swarm)

add_executable(test_dht tests/test_dht.c)
target_link_libraries(test_dht wex_core)
add_test(NAME test_dht COMMAND test_dht)

add_executable(test_nat tests/test_nat.c)
target_link_libraries(test_nat wex_core)
add_test(NAME test_nat COMMAND test_nat)
