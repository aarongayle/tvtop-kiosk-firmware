cmake_minimum_required(VERSION 3.16)

# Targets: esp32c3 (the ESP32-C3-DevKitM-1 bring-up rig) and esp32c2 (the ESP8684-MINI-1 on the v3
# board). Same code; the two modules share a land pattern and put UART0, EN and the straps on the
# same pads, so the BOM line can follow whichever part is in stock.
#   idf.py set-target esp32c3 && idf.py build flash monitor
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tvtop_modem)
