add_executable(
  otf
  otf.cpp
)

# Pull in pico libraries that we need
target_link_libraries(otf pico_stdlib pico_multicore pico_dvhstx)
pico_enable_stdio_usb(otf 1)

# create map/bin/hex file etc.
pico_add_extra_outputs(otf)
