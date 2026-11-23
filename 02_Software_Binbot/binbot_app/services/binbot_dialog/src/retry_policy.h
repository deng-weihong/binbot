#ifndef BINBOT_DIALOG_RETRY_POLICY_H
#define BINBOT_DIALOG_RETRY_POLICY_H

#include <stddef.h>
#include <stdint.h>

uint32_t binbot_retry_delay_ms(size_t attempt);

#endif
