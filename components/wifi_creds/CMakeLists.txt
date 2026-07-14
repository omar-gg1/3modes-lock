idf_component_register(SRCS "wifi_creds.c"
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "../mqtt_ctrl/include"
                       REQUIRES nvs_flash)
