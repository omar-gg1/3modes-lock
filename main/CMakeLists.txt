idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES lock_ctrl button_ctrl camera_ctrl wifi_ctrl wifi_creds qr_scan face_ctrl keypad_ctrl crypto_ctrl liveness_ctrl mqtt_ctrl cloud_verify_ctrl enroll_request temp_pin door_pin confirm_pin mode_ctrl ble_ctrl nvs_flash)