#pragma once

#include "esp_err.h"

/* Binds the listener before returning. Network credentials may be configured later. */
esp_err_t power_card_tcp_server_start(void);
