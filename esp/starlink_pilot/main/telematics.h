#pragma once

#include "esp_err.h"
#include "esp_event.h"

void telematics_start(void);
void telematics_on_nmea(void *event_handler_arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data);
