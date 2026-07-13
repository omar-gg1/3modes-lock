idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES lock_ctrl button_ctrl camera_ctrl wifi_ctrl face_ctrl keypad_ctrl crypto_ctrl liveness_ctrl mqtt_ctrl cloud_verify_ctrl enroll_request nvs_flash)