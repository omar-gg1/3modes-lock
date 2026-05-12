idf_component_register(SRCS "debug_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server log camera_ctrl)