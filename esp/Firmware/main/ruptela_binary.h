#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Parse a complete Ruptela RS232 binary record (transparent channel format).
// buf must contain the full packet including the 4-byte preamble/length header
// and the trailing CRC16. Returns true if CRC matched and at least one record
// was decoded.
bool ruptela_record_parse(const uint8_t *buf, size_t len);
