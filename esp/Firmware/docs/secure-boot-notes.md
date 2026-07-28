# Cómo bloquear el ESP32-S3 para que nadie pueda leer ni modificar el firmware

Notas de investigación (no implementado todavía). Objetivo: evitar que alguien con acceso físico a la placa pueda (1) flashear un firmware distinto al nuestro, o (2) leer/copiar el firmware actual.

## Estado actual del proyecto (verificado)
- Chip: ESP32-S3, IDF v5.4.4.
- Ninguna protección está activada hoy: `sdkconfig` tiene `SECURE_BOOT`, `SECURE_FLASH_ENC_ENABLED` y `FLASH_ENCRYPTION_ENABLED` en `not set`.
- No hay claves de firma en el repo.
- `partitions.csv` no reserva espacio extra de bootloader (necesario para Secure Boot/Flash Encryption).

## Los 3 mecanismos del ESP32-S3

### 1. Secure Boot V2 → evita firmware ajeno
- El bootloader y la app se firman con una clave RSA privada (offline, nunca en el repo).
- La clave pública (o su hash) se quema en un eFuse.
- En cada arranque se verifica la firma; si no coincide, el chip no arranca esa imagen.
- **Irreversible** una vez quemado `SECURE_BOOT_EN`. Si se pierde la clave privada, no se puede volver a actualizar el firmware.

### 2. Flash Encryption → evita leer/copiar el firmware
- Cifra todo el contenido de la flash (app, NVS, particiones) con una clave AES-256 generada dentro del chip y no legible por software.
- Volcar la flash con `esptool` solo da bytes cifrados inútiles.
- Modo Development (permite reflashear en pruebas) vs modo Release (definitivo, irreversible).
- Se recomienda combinar con Secure Boot: uno da autenticidad, el otro confidencialidad.

### 3. Bloqueo de JTAG / descarga UART → cierra el acceso físico de bajo nivel
- Por defecto se puede entrar en modo descarga UART/USB y usar `esptool.py` sin pasar por Secure Boot ni Flash Encryption.
- Al activar Flash Encryption, el propio dispositivo quema automáticamente en el primer boot: `DIS_DOWNLOAD_MANUAL_ENCRYPT`, `DIS_DOWNLOAD_ICACHE`, `DIS_DOWNLOAD_DCACHE`, `HARD_DIS_JTAG`, `DIS_USB_JTAG`. Es decir, este paso ya viene incluido al activar Flash Encryption con la configuración por defecto.
- En cuanto se activa cualquier función de seguridad, el modo descarga UART pasa a "Secure UART Download Mode": ya no permite ejecutar código arbitrario, solo comandos limitados. Para bloquearlo del todo existe `CONFIG_SECURE_UART_ROM_DL_MODE` → "Permanently disable ROM Download Mode".
- El JTAG se puede "soft-disable" (reactivable con una clave HMAC secreta) o "hard-disable" permanente vía `HARD_DIS_JTAG`/`DIS_USB_JTAG`.

## Verificación contra documentación oficial de Espressif
- Espressif recomienda explícitamente combinar Secure Boot con Flash Encryption "to prevent local readout of the flash contents". ([Secure Boot V2 - ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html), [Flash Encryption - ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/flash-encryption.html))
- Confirmado el comportamiento automático de eFuses al activar Flash Encryption. ([Security Overview - ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/security.html))
- Para producción en serie (no aplica a una sola placa personal): usar hasta 3 claves RSA-3072 generadas con OpenSSL, quemar `KEY_REVOKE` de slots no usados antes de salir de fábrica.
- Activar Secure Boot y/o Flash Encryption aumenta el tamaño del bootloader → puede requerir ajustar el offset en `partitions.csv`.

**Conclusión**: Secure Boot V2 + Flash Encryption (+ bloqueo JTAG/UART, que en gran parte viene incluido) es el camino recomendado y oficial de Espressif para ESP32-S3. No hay una alternativa mejor documentada.

## Riesgos antes de implementar
- **Irreversible**: quemar estos eFuses (sobre todo en modo Release) puede dejar la placa inutilizable si algo se configura mal.
- **Gestión de claves**: perder la clave privada de firma implica no poder volver a actualizar el firmware nunca más.
- **Fricción de desarrollo**: cada flash de prueba debe firmarse/cifrarse. Recomendado probar todo en modo Development sobre una placa de repuesto antes de pasar a Release en la placa final.
- Nada de esto está activado todavía — este documento es solo la investigación/explicación, sin cambios en el proyecto.
