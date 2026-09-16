cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico_w CACHE STRING "")
if (NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.2.0")
endif()
if (NOT DEFINED ENV{PICO_TOOLCHAIN_PATH} AND NOT PICO_TOOLCHAIN_PATH)
    set(PICO_TOOLCHAIN_PATH "$ENV{HOME}/.pico-sdk/toolchain/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi")
endif()
include(pico_sdk_import.cmake)
project(usb_test C CXX ASM)
pico_sdk_init()

# The same trivial USB serial program twice: once running from flash (normal), once loaded
# straight into SRAM by picotool and never touching flash. If only the RAM one enumerates, the
# flash chip or its XIP boot path is the problem.
foreach(kind flash ram)
    add_executable(usb_hello_${kind} main.c)
    target_compile_definitions(usb_hello_${kind} PRIVATE BUILD_KIND="${kind}")
    target_link_libraries(usb_hello_${kind} pico_stdlib)
    pico_enable_stdio_usb(usb_hello_${kind} 1)
    pico_enable_stdio_uart(usb_hello_${kind} 0)
    pico_add_extra_outputs(usb_hello_${kind})
endforeach()
pico_set_binary_type(usb_hello_ram no_flash)
