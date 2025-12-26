add_library(audio_driver INTERFACE)

target_sources(audio_driver INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/audio_i2s_driver.c
)

target_include_directories(audio_driver INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(audio_driver INTERFACE
    pico_audio_i2s
    pico_sync
    pico_multicore
    hardware_gpio
)
