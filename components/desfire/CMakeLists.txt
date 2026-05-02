idf_component_register(
    SRCS
        "desfire.c"
        "desfire_auth.c"
        "desfire_crypto.c"
        "desfire_enc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES log mbedtls nfc_hal esp_hw_support
)
