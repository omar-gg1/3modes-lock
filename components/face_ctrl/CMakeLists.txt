idf_component_register(
    SRCS "face_ctrl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp32-camera
        esp_new_jpeg
        esp-dl
        human_face_detect
        human_face_recognition
        spiffs
        log
        esp_psram
)