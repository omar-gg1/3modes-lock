idf_component_register(SRCS "cmd_verify.c" "mqtt_ctrl.c"
                       INCLUDE_DIRS "include" "."
                       REQUIRES mqtt esp_timer esp_netif lwip log
                                mbedtls json lock_ctrl)
