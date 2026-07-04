idf_component_register(SRCS "cloud_verify_ctrl.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client camera_ctrl mqtt_ctrl log)
