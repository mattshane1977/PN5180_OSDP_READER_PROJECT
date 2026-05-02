idf_component_register(
    SRCS "webui.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/index.html"
    REQUIRES esp_http_server log freertos esp_netif nvs_flash
)
