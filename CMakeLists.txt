cmake_minimum_required(VERSION 3.13)

# ---- Build configuration (override with -D) ----
set(PICO_BOARD pico_w CACHE STRING "pico_w | pico2_w")
option(KIOSK_TLS "Build HTTPS (mbedTLS) support" OFF)
set(KIOSK_VIDEO_MODE "720p30" CACHE STRING "Default video mode: 720p30 | 720p30rb | 480p60")
set(KIOSK_SERVER_BASE "https://kiosk.tvtop.games" CACHE STRING "Default kiosk server base URL")
set(KIOSK_WIFI_SSID "" CACHE STRING "Optional compiled-in Wi-Fi SSID (provisioning otherwise)")
set(KIOSK_WIFI_PASSWORD "" CACHE STRING "Optional compiled-in Wi-Fi password")

if (NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.2.0")
endif()
if (NOT DEFINED ENV{PICO_TOOLCHAIN_PATH} AND NOT PICO_TOOLCHAIN_PATH)
    set(PICO_TOOLCHAIN_PATH "$ENV{HOME}/.pico-sdk/toolchain/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi")
endif()

# The RP2040 runs at up to 372 MHz; the flash must stay under ~100 MHz (clk_sys/4).
add_compile_definitions(PICO_FLASH_SPI_CLKDIV=4)

include(pico_sdk_import.cmake)
project(tvtop_kiosk C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_subdirectory(vendor/libdvi)

file(GLOB KIOSK_COMMON_SRC ${CMAKE_CURRENT_LIST_DIR}/src/common/*.c)
file(GLOB KIOSK_PICO_SRC ${CMAKE_CURRENT_LIST_DIR}/src/pico/*.c ${CMAKE_CURRENT_LIST_DIR}/src/pico/*.S)
list(FILTER KIOSK_COMMON_SRC EXCLUDE REGEX "geom_ram\\.c$")

add_executable(tvtop_kiosk ${KIOSK_COMMON_SRC} ${KIOSK_PICO_SRC}
    vendor/pico-examples/dhcpserver/dhcpserver.c
    vendor/pico-examples/dnsserver/dnsserver.c)

target_include_directories(tvtop_kiosk PRIVATE
    src/common src/pico vendor/libdvi vendor/pico-examples/dhcpserver vendor/pico-examples/dnsserver)

target_compile_options(tvtop_kiosk PRIVATE -Wall -Wextra -Wno-unused-parameter -O2 -fstack-usage)

target_compile_definitions(tvtop_kiosk PRIVATE
    DVI_DEFAULT_SERIAL_CONFIG=pico_sock_cfg
    DVI_VERTICAL_REPEAT=1
    DVI_N_TMDS_BUFFERS=0          # we allocate the TMDS line buffers statically
    DVI_SYMBOLS_PER_WORD=2
    CYW43_PIO_CLOCK_DIV_DYNAMIC=1 # SPI divider chosen at runtime from the overclocked sys clock
    PICO_STACK_SIZE=0x1000          # the SDK memmap keeps the core-0 stack in the 4 KB SCRATCH_Y bank
    PICO_CORE1_STACK_SIZE=0x800
    KIOSK_TLS=$<BOOL:${KIOSK_TLS}>
    KIOSK_DEFAULT_VIDEO_MODE_NAME="${KIOSK_VIDEO_MODE}"
    KIOSK_DEFAULT_SERVER_BASE="${KIOSK_SERVER_BASE}"
    KIOSK_DEFAULT_WIFI_SSID="${KIOSK_WIFI_SSID}"
    KIOSK_DEFAULT_WIFI_PASSWORD="${KIOSK_WIFI_PASSWORD}"
    PICO_CYW43_ARCH_DEBUG_ENABLED=0
    ALTCP_MBEDTLS_AUTHMODE=MBEDTLS_SSL_VERIFY_REQUIRED
)

if (KIOSK_TLS)
    target_compile_definitions(tvtop_kiosk PRIVATE KIOSK_LINEPOOL_BYTES=57344)
endif()

target_link_libraries(tvtop_kiosk
    pico_stdlib pico_multicore pico_util pico_rand pico_unique_id
    hardware_dma hardware_pio hardware_pwm hardware_interp hardware_vreg hardware_flash hardware_xip_cache hardware_sync
    pico_cyw43_arch_lwip_threadsafe_background
    libdvi)
if (KIOSK_TLS)
    target_link_libraries(tvtop_kiosk pico_lwip_mbedtls pico_mbedtls)
endif()

pico_enable_stdio_usb(tvtop_kiosk 1)
pico_enable_stdio_uart(tvtop_kiosk 0)
pico_add_extra_outputs(tvtop_kiosk)
