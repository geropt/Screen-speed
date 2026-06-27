#pragma once

#include <stdbool.h>

// Configure the BOOT button (GPIO0) and start the polling task.
void buttons_init(void);

// Returns true (once) if the BOOT button was held long enough to request WiFi
// provisioning, clearing the request. Polled from the main loop, which owns the
// heavyweight provisioning flow (SoftAP portal + reboot).
bool buttons_take_provisioning_request(void);
