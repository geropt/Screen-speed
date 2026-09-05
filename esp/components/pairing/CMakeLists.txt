# Contratos de vinculación con el teléfono: lógica pura, sin BLE. El transporte se
# implementa aparte, sobre la placa. Se prueba en el host.
idf_component_register(SRCS "pairing.c"
                       INCLUDE_DIRS "include")
