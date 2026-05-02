idf_component_register(
    SRCS "board_io.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_timer log led_strip
)
