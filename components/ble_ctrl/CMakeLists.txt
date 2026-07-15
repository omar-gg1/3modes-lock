idf_component_register(SRCS "ble_ctrl.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt json nvs_flash mqtt_ctrl lock_ctrl)
