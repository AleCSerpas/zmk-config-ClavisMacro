# ClavisMacro custom firmware components.

target_include_directories(app PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

zephyr_library_include_directories(
    ${CMAKE_SOURCE_DIR}/include
)

zephyr_library_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

zephyr_library_sources(
    clavis_rgb_engine.c
    clavis_rgb_effects.c
    clavis_rgb_key_events.c
    clavis_screen_config.c
    behavior_clavis_rgb.c
    clavis_hall_kscan.c
    clavis_raw_hid.c
)

# Custom ST7789 status screen.
if(CONFIG_ZMK_DISPLAY AND CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
    zephyr_library_include_directories(
        ${CMAKE_SOURCE_DIR}/include
    )

    zephyr_library_sources(
        clavis_status_screen.c
    )
endif()
