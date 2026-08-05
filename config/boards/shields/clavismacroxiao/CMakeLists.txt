# ClavisMacro custom RGB Matrix engine.
# The old clavis_rgb_test.c is intentionally not compiled anymore.
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
zephyr_library_sources(
    clavis_rgb_engine.c
    behavior_clavis_rgb.c
)

# Existing custom screen. This stays untouched and compiles only when enabled.
if(CONFIG_ZMK_DISPLAY AND CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
    zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
    zephyr_library_sources(clavis_status_screen.c)
endif()
