cmake_minimum_required(VERSION 3.16)
project(wex_chat C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings and strict sanitizers in Debug
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wstrict-prototypes)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

include_directories(include)

# Wex Core Library Sources (to be populated)
set(WEX_CORE_SOURCES
    src/main.c
)

add_executable(wex_daemon ${WEX_CORE_SOURCES})

# Enable testing
enable_testing()
