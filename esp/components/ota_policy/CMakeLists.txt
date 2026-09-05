# Decisiones de OTA: qué imagen se acepta, cuándo se confirma, cuándo se vuelve. Lógica
# pura; escribir el slot y seleccionar el boot los hace el firmware con esp_ota_*.
idf_component_register(SRCS "ota_policy.c"
                       INCLUDE_DIRS "include")
