zephyr_library()

zephyr_library_sources_ifdef(
    CONFIG_CUSTOM_LED
    src/custom_led.c
)

zephyr_library_sources_ifdef(
    CONFIG_CUSTOM_TOUCH
    src/custom_touch.c
)
