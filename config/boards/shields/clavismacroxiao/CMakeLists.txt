if(CONFIG_ZMK_DISPLAY AND CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
    # Custom shield sources are compiled as a Zephyr library. Add ZMK's
    # application-private headers so includes such as <zmk/battery.h>,
    # <zmk/keymap.h>, and <zmk/rgb_underglow.h> can be resolved.
    zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
    zephyr_library_sources(clavis_status_screen.c)
endif()
