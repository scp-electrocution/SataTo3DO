cmake_minimum_required(VERSION 3.13)
include(pico-sdk/pico_sdk_init.cmake)

set(FAMILY rp2040)
set(BOARD pico_sdk)
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tinyusb")

# Overridable from the command line (-DPICO_BOARD=pico for an official Pi Pico)
if(NOT DEFINED PICO_BOARD)
  set(PICO_BOARD waveshare_rp2040_plus_4mb)
endif()

set(PICO_STACK_SIZE _u(0x1000))

include(${PICO_TINYUSB_PATH}/hw/bsp/${FAMILY}/family.cmake)

set(CFG_TUSB_CONFIG_FILE "include/interfaces/tusb_config.h")


project(SataTo3DO)

add_definitions(-DPICO_TINYUSB_PATH=${PICO_TINYUSB_PATH})

pico_sdk_init()

include_directories(./FATFs)
include_directories(./include)
include_directories(./include/controllers)
include_directories(./include/interfaces)
include_directories(./include/gamepads)



add_executable(SataTo3DO)

target_sources(SataTo3DO PRIVATE
src/main.c
src/interfaces/USB.c
src/interfaces/3DO.c
src/interfaces/CDROM.c
src/interfaces/MSC.c
FATFs/diskio.c
FATFs/ff.c
FATFs/ffunicode.c
)

pico_generate_pio_header(SataTo3DO ${CMAKE_CURRENT_LIST_DIR}/src/interfaces/read.pio)

# set(PICO_STACK_SIZE ...) above is only a CMake variable and has no effect:
# the stack sizes have to be compile definitions. Both stacks were 2KB, which
# is too small (a 2449 bytes sector buffer, FatFs objects...).
target_compile_definitions(SataTo3DO PRIVATE
  PICO_STACK_SIZE=0x1000
  PICO_CORE1_STACK_SIZE=0x1000
)

# Warnings on the project sources only
set_source_files_properties(
  src/main.c
  src/interfaces/USB.c
  src/interfaces/3DO.c
  src/interfaces/CDROM.c
  src/interfaces/MSC.c
  FATFs/diskio.c
  PROPERTIES COMPILE_OPTIONS "-Wall")

# enable usb output, disable uart output
# pico_enable_stdio_usb(SataTo3DO 1)
option(USE_DEBUG "Use the UART RX/TX" OFF)
if (USE_DEBUG)
  add_definitions("-DUSE_TRACE")
  pico_enable_stdio_uart(SataTo3DO 1)
endif()

option(USE_DIR_IN_HIGH "Data direction signal is 1 when read" OFF)
if (USE_DIR_IN_HIGH)
add_definitions("-DDIR_IN_HI")
message("DIR is High polarity")
else()
message("DIR is Low polarity")
endif()

option(USE_HS_FC "Use the HW version with a hw bug on CTRL3 pin" OFF)
if (USE_HS_FC)
add_definitions("-DHW_HS_FC")
pico_generate_pio_header(SataTo3DO ${CMAKE_CURRENT_LIST_DIR}/src/interfaces/write_hs_fc.pio)
else()
pico_generate_pio_header(SataTo3DO ${CMAKE_CURRENT_LIST_DIR}/src/interfaces/write.pio)
endif()

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(SataTo3DO)

target_link_libraries(SataTo3DO PRIVATE pico_stdlib hardware_pio hardware_dma pico_multicore tinyusb_board tinyusb_host)