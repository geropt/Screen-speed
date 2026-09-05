# Lógica de presentación pura: decide QUÉ mostrar, no cómo dibujarlo. Sin LVGL,
# sin ESP-IDF, sin memoria dinámica; el reloj llega dentro del snapshot. Se prueba
# en el host (host_tests/test_ui_model.c).
idf_component_register(SRCS "ui_model.c"
                       INCLUDE_DIRS "include"
                       REQUIRES vehicle_state)
