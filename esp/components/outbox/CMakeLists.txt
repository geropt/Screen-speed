# C puro, sin ESP-IDF: el reloj entra por parámetro y no hay memoria dinámica.
# Compartido por el HUD y el piloto. Se prueba en el host.
idf_component_register(SRCS "outbox.c"
                            "backup_policy.c"
                       INCLUDE_DIRS "include")
