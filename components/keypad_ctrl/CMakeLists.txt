idf_component_register(
    SRCS "keypad_ctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES driver log esp_timer
)