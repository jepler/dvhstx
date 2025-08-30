
include(drivers/dvhstx/dvhstx)

set(LIB_NAME pico_dvhstx)
add_library(${LIB_NAME} INTERFACE)

target_sources(${LIB_NAME} INTERFACE
    ${PIMORONI_PICO_PATH}/libraries/pico_graphics/types.cpp
)

target_include_directories(${LIB_NAME} INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(${LIB_NAME} INTERFACE dvhstx pico_stdlib hardware_i2c hardware_dma)
