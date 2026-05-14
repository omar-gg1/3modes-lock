idf_component_register(
    SRCS "face_ctrl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp32-camera
        esp_new_jpeg
        esp-dl
        human_face_detect
        log
        esp_psram
)