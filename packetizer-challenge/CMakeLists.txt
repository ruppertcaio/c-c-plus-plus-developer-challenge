cmake_minimum_required(VERSION 3.16)
project(packetizer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wstrict-prototypes
        -Wmissing-prototypes
    )
endif()

option(PKT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(PKT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# No .c files yet, so the core is header-only. Flip to add_library(packetizer STATIC ...)
# once lib/packetizer/src gains sources, and move this include dir to PUBLIC.
add_library(packetizer INTERFACE)
target_include_directories(packetizer INTERFACE lib/packetizer/include)

enable_testing()

# One test binary per call: pkt_add_test(name src1.c [src2.c ...]).
function(pkt_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE packetizer)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pkt_add_test(test_smoke tests/test_smoke.c)
