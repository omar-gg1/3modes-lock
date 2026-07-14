idf_component_register(SRCS "qr_scan.c" "qr_parse.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp32-camera wifi_creds espressif__quirc log)
