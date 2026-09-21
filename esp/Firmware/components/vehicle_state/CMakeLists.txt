# C puro, sin dependencias de ESP-IDF: el reloj entra por parámetro y no hay
# memoria dinámica ni locks. Se prueba en el host (host_tests/test_vehicle_state.c).
idf_component_register(SRCS "vehicle_state.c"
                       INCLUDE_DIRS "include")
