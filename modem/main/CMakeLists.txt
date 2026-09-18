idf_component_register(
    SRCS "main.c" "link.c" "net.c" "httpc.c" "portal.c"
    # ../../src/common carries modem_proto.h, the one copy of the wire format both firmwares build.
    INCLUDE_DIRS "." "../../src/common"
    REQUIRES driver esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls lwip nvs_flash esp_hw_support)

# Reported to the kiosk in M_HELLO and printed on its USB console.
target_compile_definitions(${COMPONENT_LIB} PRIVATE MODEM_FW_VERSION="0.1.0")

# The bring-up rig keeps the link off UART0 so the DevKit's USB console still works; the v3 board
# has no second USB port and puts the link on UART0, which is also the ROM bootloader.
#   idf.py -DMODEM_LINK_UART=0 -DMODEM_LINK_TX_PIN=21 -DMODEM_LINK_RX_PIN=20 build
foreach(opt MODEM_LINK_UART MODEM_LINK_TX_PIN MODEM_LINK_RX_PIN MODEM_BAUD)
    if (DEFINED ${opt})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${opt}=${${opt}})
    endif()
endforeach()
