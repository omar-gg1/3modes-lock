idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES lock_ctrl button_ctrl camera_ctrl wifi_ctrl debug_server face_ctrl keypad_ctrl nvs_flash)