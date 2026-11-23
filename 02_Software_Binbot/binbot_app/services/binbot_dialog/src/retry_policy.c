#include "retry_policy.h"

uint32_t binbot_retry_delay_ms(size_t attempt)
{
    static const uint32_t delays[] = {1000, 2000, 4000, 8000, 16000, 30000};
    if(attempt >= sizeof(delays) / sizeof(delays[0])) {
        return delays[sizeof(delays) / sizeof(delays[0]) - 1];
    }
    return delays[attempt];
}
