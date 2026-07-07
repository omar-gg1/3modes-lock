idf_component_register(SRCS "cloud_verify_ctrl.c"
                       INCLUDE_DIRS "include"
                       # face_ctrl: the retained murky-scored frame
                       # mqtt_ctrl:  wifi_config.h (VERIFIER_URL / API key)
                       REQUIRES esp_http_client face_ctrl mqtt_ctrl log)
