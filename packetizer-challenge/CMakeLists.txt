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

add_library(packetizer STATIC
    lib/packetizer/src/crc.c
    lib/packetizer/src/cobs.c
    lib/packetizer/src/frame.c
    lib/packetizer/src/pkt.c
)
target_include_directories(packetizer PUBLIC lib/packetizer/include)

enable_testing()

# One test binary per call: pkt_add_test(name src1.c [src2.c ...]).
function(pkt_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE packetizer)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pkt_add_test(test_smoke tests/test_smoke.c)
pkt_add_test(test_crc tests/test_crc.c)
pkt_add_test(test_cobs tests/test_cobs.c)
pkt_add_test(test_frame tests/test_frame.c)
pkt_add_test(test_reassembly tests/test_reassembly.c tests/loopback.c)
pkt_add_test(test_reliability tests/test_reliability.c tests/loopback.c)
