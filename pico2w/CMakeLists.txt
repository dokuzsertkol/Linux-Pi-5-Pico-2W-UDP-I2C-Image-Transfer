add_executable(pico2w
    pico2w.c
)

target_include_directories(pico2w PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pico2w PRIVATE
    pico_stdlib
    hardware_i2c
    pico_i2c_slave
)

pico_add_extra_outputs(pico2w)
pico_enable_stdio_usb(pico2w 1)