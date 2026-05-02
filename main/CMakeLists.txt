idf_component_register(
    SRCS
        "main.c"
        "reader_task.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        driver
        log
        board
        nfc_hal
        pn5180
        desfire
        netconfig
        webui
)
