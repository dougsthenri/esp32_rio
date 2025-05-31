idf_component_register(SRCS "wifi_connect.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_wifi nvs_flash)