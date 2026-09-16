cmake_minimum_required(VERSION 3.13)
project(tvtop_kiosk_host C)
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2 -g)

set(COMMON ${CMAKE_CURRENT_LIST_DIR}/../src/common)
file(GLOB COMMON_SRC ${COMMON}/*.c)
list(FILTER COMMON_SRC EXCLUDE REGEX "geom_flash\\.c$")

add_library(kiosk_common STATIC ${COMMON_SRC})
target_include_directories(kiosk_common PUBLIC ${COMMON} ${CMAKE_CURRENT_LIST_DIR}/../vendor/stb)
# OUT_MAX 1920x1080 so the renderer and tests cover the Pico 2 W's 1080p modes as well as 720p.
target_compile_definitions(kiosk_common PUBLIC KIOSK_HOST=1 KIOSK_BAND_LINES=16 KIOSK_SCRATCH_BYTES=262144 KIOSK_MAX_OPS=4096 KIOSK_ARENA_BYTES=65536 OUT_MAX_W=1920 OUT_MAX_H=1080)

# The device encoder is portable C with an asm fast path; the C version is tested here.
add_library(kiosk_encoder STATIC ${CMAKE_CURRENT_LIST_DIR}/../src/pico/tmds_wr.c ${CMAKE_CURRENT_LIST_DIR}/../src/pico/hstx_line.c)
target_include_directories(kiosk_encoder PUBLIC ${COMMON} ${CMAKE_CURRENT_LIST_DIR}/../src/pico)
target_compile_definitions(kiosk_encoder PUBLIC KIOSK_HOST=1)

add_executable(render_frame render_frame.c)
target_link_libraries(render_frame kiosk_common kiosk_encoder m)
add_executable(imgdiff imgdiff.c)
target_include_directories(imgdiff PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../vendor/stb)
target_link_libraries(imgdiff m)

enable_testing()
file(GLOB TEST_SRC ${CMAKE_CURRENT_LIST_DIR}/tests/test_*.c)
# Tests that include device sources directly (with host fakes) or need a different backend.
set(SPECIAL_TESTS test_geom_flash test_flash_store test_provision)
foreach(t ${TEST_SRC})
    get_filename_component(name ${t} NAME_WE)
    if (name IN_LIST SPECIAL_TESTS)
        continue()
    endif()
    add_executable(${name} ${t})
    target_link_libraries(${name} kiosk_common kiosk_encoder m)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..)
endforeach()

# geom_flash.c over a fake flash: links the flash backend instead of geom_ram.c.
add_executable(test_geom_flash tests/test_geom_flash.c ${COMMON}/geom_flash.c ${COMMON}/geom_common.c)
target_include_directories(test_geom_flash PRIVATE ${COMMON} ${CMAKE_CURRENT_LIST_DIR}/../src/pico)
target_compile_definitions(test_geom_flash PRIVATE KIOSK_HOST=1 STANDALONE_TEST=1)
add_test(NAME test_geom_flash COMMAND test_geom_flash WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..)

# Device modules compiled on the host with their transport/flash faked (they #include the .c).
foreach(name test_flash_store test_provision)
    add_executable(${name} tests/${name}.c)
    target_include_directories(${name} PRIVATE ${COMMON} ${CMAKE_CURRENT_LIST_DIR}/../src/pico)
    target_compile_definitions(${name} PRIVATE KIOSK_HOST=1)
    target_link_libraries(${name} kiosk_common m)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..)
endforeach()
