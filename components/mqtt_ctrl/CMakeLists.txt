idf_component_register(SRCS "mqtt_ctrl.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqtt esp_timer esp_netif lwip log)
