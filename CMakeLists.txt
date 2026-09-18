cmake_minimum_required(VERSION 3.13)

# ---- Build configuration (override with -D) ----
set(PICO_BOARD pico_w CACHE STRING "pico_w | pico2_w")
option(KIOSK_TLS "Build HTTPS (mbedTLS) support" OFF)
option(KIOSK_NO_RADIO "Diagnostic build: never start the Wi-Fi radio" OFF)
# v3 replaces the on-board radio with an ESP32 module on a UART: the modem owns Wi-Fi, the IP
# stack and TLS, and this firmware talks to it through src/pico/modem_link.c. Also buildable on a
# Pico 2 W wired to an ESP32-C3 DevKit (docs/MODEM.md), which is how it is developed.
option(KIOSK_MODEM "Use an ESP32 modem over UART instead of the on-board cyw43 radio" OFF)
set(KIOSK_VIDEO_MODE "" CACHE STRING "Default video mode until one is stored. Pico W: 720p30 | 720p30rb | 480p60 | 720x480p60 | 960x540p60 | 1066x600p50 (default 720p30). Pico 2 W: 720p60 | 960x540p60 | 480p60 (default 720p60)")
set(KIOSK_SERVER_BASE "https://kiosk.tvtop.games" CACHE STRING "Default kiosk server base URL")
# Whether a network really reaches the internet is decided by fetching this and expecting 204 with
# no body; anything else means something is answering on the network's behalf (a sign-in portal).
# It must be plain HTTP — a portal cannot intercept TLS, it can only break it, which is
# indistinguishable from the server being down. The kiosk sends this to the modem per probe.
set(KIOSK_NET_CHECK_URL "" CACHE STRING "Connectivity probe URL (default: Google's generate_204)")
set(KIOSK_WIFI_SSID "" CACHE STRING "Optional compiled-in Wi-Fi SSID (provisioning otherwise)")
set(KIOSK_WIFI_PASSWORD "" CACHE STRING "Optional compiled-in Wi-Fi password")

if (NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.2.0")
endif()
if (NOT DEFINED ENV{PICO_TOOLCHAIN_PATH} AND NOT PICO_TOOLCHAIN_PATH)
    set(PICO_TOOLCHAIN_PATH "$ENV{HOME}/.pico-sdk/toolchain/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi")
endif()

# The RP2040 runs at up to 372 MHz; its boot stage 2 keeps flash under ~100 MHz (clk_sys/4). The
# RP2350 has no boot stage 2: scanout_hstx.c raises the flash divisor itself before overclocking.
add_compile_definitions(PICO_FLASH_SPI_CLKDIV=4)

include(pico_sdk_import.cmake)
project(tvtop_kiosk C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# The Pico 2 W's RP2350 has HSTX, a hardware serialiser with a TMDS encoder (scanout_hstx.c). The
# Pico W's RP2040 drives DVI from PIO through PicoDVI's libdvi (scanout.c).
if (PICO_PLATFORM MATCHES "^rp2350")
    set(KIOSK_HSTX 1)
else()
    set(KIOSK_HSTX 0)
endif()
if (KIOSK_VIDEO_MODE STREQUAL "")
    if (KIOSK_HSTX)
        set(KIOSK_DEFAULT_MODE "720p60")
    else()
        set(KIOSK_DEFAULT_MODE "720p30")
    endif()
else()
    set(KIOSK_DEFAULT_MODE "${KIOSK_VIDEO_MODE}")
endif()
message(STATUS "tvtop kiosk: board ${PICO_BOARD}, HSTX ${KIOSK_HSTX}, default video mode ${KIOSK_DEFAULT_MODE}")

if (NOT KIOSK_HSTX)
    add_subdirectory(vendor/libdvi)
endif()

file(GLOB KIOSK_COMMON_SRC ${CMAKE_CURRENT_LIST_DIR}/src/common/*.c)
file(GLOB KIOSK_PICO_SRC ${CMAKE_CURRENT_LIST_DIR}/src/pico/*.c ${CMAKE_CURRENT_LIST_DIR}/src/pico/*.S)
list(FILTER KIOSK_COMMON_SRC EXCLUDE REGEX "geom_ram\\.c$")
if (KIOSK_HSTX)
    list(FILTER KIOSK_PICO_SRC EXCLUDE REGEX "/(scanout\\.c|tmds_wr\\.c|tmds_wr_expand_m0\\.S)$")
else()
    list(FILTER KIOSK_PICO_SRC EXCLUDE REGEX "/(scanout_hstx\\.c|hstx_line\\.c)$")
endif()
# The two network backends are mutually exclusive: each provides net_wifi.h and http_client.h.
if (KIOSK_MODEM)
    list(FILTER KIOSK_PICO_SRC EXCLUDE REGEX "/(net_wifi\\.c|http_client\\.c)$")
    set(KIOSK_NET_SRC "")   # DHCP and DNS servers live on the modem
else()
    list(FILTER KIOSK_PICO_SRC EXCLUDE REGEX "/(net_modem\\.c|http_modem\\.c|modem_link\\.c)$")
    set(KIOSK_NET_SRC
        vendor/pico-examples/dhcpserver/dhcpserver.c
        vendor/pico-examples/dnsserver/dnsserver.c)
endif()

add_executable(tvtop_kiosk ${KIOSK_COMMON_SRC} ${KIOSK_PICO_SRC} ${KIOSK_NET_SRC})

target_include_directories(tvtop_kiosk PRIVATE
    src/common src/pico vendor/pico-examples/dhcpserver vendor/pico-examples/dnsserver)
if (NOT KIOSK_HSTX)
    target_include_directories(tvtop_kiosk PRIVATE vendor/libdvi)
endif()

target_compile_options(tvtop_kiosk PRIVATE -Wall -Wextra -Wno-unused-parameter -O2 -fstack-usage)

# Reported to the server at registration and in the User-Agent: pico-w or pico2-w.
string(REPLACE "_" "-" KIOSK_MODEL "${PICO_BOARD}")

target_compile_definitions(tvtop_kiosk PRIVATE
    CYW43_PIO_CLOCK_DIV_DYNAMIC=1 # SPI divider chosen at runtime from the overclocked sys clock
    KIOSK_MODEM=$<BOOL:${KIOSK_MODEM}>
    $<$<BOOL:${KIOSK_NET_CHECK_URL}>:MODEM_NET_CHECK_URL="${KIOSK_NET_CHECK_URL}">
    KIOSK_MODEL="${KIOSK_MODEL}"
    KIOSK_TLS=$<BOOL:${KIOSK_TLS}>
    KIOSK_NO_RADIO=$<BOOL:${KIOSK_NO_RADIO}>
    KIOSK_HSTX=${KIOSK_HSTX}
    KIOSK_DEFAULT_VIDEO_MODE_NAME="${KIOSK_DEFAULT_MODE}"
    KIOSK_DEFAULT_SERVER_BASE="${KIOSK_SERVER_BASE}"
    KIOSK_DEFAULT_WIFI_SSID="${KIOSK_WIFI_SSID}"
    KIOSK_DEFAULT_WIFI_PASSWORD="${KIOSK_WIFI_PASSWORD}"
    PICO_CYW43_ARCH_DEBUG_ENABLED=0
    ALTCP_MBEDTLS_AUTHMODE=MBEDTLS_SSL_VERIFY_REQUIRED
)

if (KIOSK_HSTX)
    target_compile_definitions(tvtop_kiosk PRIVATE
        KIOSK_FULL_COLOUR=1           # HSTX encodes any 8-bit value: palette colours stay exact
        KIOSK_ASM_EXPANDER=0
        KIOSK_LINEPOOL_BYTES=262144   # 256 KB of the RP2350's 520 KB; a detailed board needs ~115 KB at 720p, ~230 KB at 1080p
        KIOSK_CONFIG_FLASH_OFFSET=0x3FF000u   # last sector of the Pico 2 W's 4 MB flash
        PICO_STACK_SIZE=0x1000        # the RP2350 memmap also keeps the core-0 stack in 4 KB of SCRATCH_Y
        PICO_CORE1_STACK_SIZE=0x1000
    )
else()
    target_compile_definitions(tvtop_kiosk PRIVATE
        DVI_DEFAULT_SERIAL_CONFIG=pico_sock_cfg
        DVI_VERTICAL_REPEAT=1
        DVI_N_TMDS_BUFFERS=0          # we allocate the TMDS line buffers statically
        DVI_SYMBOLS_PER_WORD=2
        PICO_STACK_SIZE=0x1000        # the SDK memmap keeps the core-0 stack in the 4 KB SCRATCH_Y bank
        PICO_CORE1_STACK_SIZE=0x800
        KIOSK_ASM_EXPANDER=1          # src/pico/tmds_wr_expand_m0.S, checked against the C version at boot
    )
    if (KIOSK_TLS)
        target_compile_definitions(tvtop_kiosk PRIVATE KIOSK_LINEPOOL_BYTES=57344)
    endif()
endif()

target_link_libraries(tvtop_kiosk
    pico_stdlib pico_multicore pico_util pico_rand pico_unique_id
    hardware_dma hardware_pio hardware_vreg hardware_flash hardware_xip_cache hardware_sync hardware_irq hardware_clocks)
if (KIOSK_MODEM)
    target_link_libraries(tvtop_kiosk hardware_uart)
else()
    target_link_libraries(tvtop_kiosk pico_cyw43_arch_lwip_threadsafe_background)
endif()
if (NOT KIOSK_HSTX)
    target_link_libraries(tvtop_kiosk hardware_pwm hardware_interp libdvi)
endif()
if (KIOSK_TLS)
    target_link_libraries(tvtop_kiosk pico_lwip_mbedtls pico_mbedtls)
endif()

pico_enable_stdio_usb(tvtop_kiosk 1)
pico_enable_stdio_uart(tvtop_kiosk 0)
pico_add_extra_outputs(tvtop_kiosk)
