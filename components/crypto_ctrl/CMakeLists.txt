idf_component_register(SRCS "crypto_ctrl.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash mbedtls log)
