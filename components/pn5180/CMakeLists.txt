idf_component_register(
    SRCS
        "pn5180_spi.c"
        "pn5180_transceive.c"
        "pn5180_iso14443.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES driver freertos esp_timer log board nfc_hal
)
