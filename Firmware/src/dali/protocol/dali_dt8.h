/*
    dali_dt8.h - DT8 colour control (IEC 62386-209)

    RGBWAF primaries + colour temperature Tc. Extended commands (224–254)
    processed when preceded by ENABLE DEVICE TYPE 8.
    Conditionally compiled: entire module guarded by EVG_HAS_DT8.
*/
#ifndef _DALI_DT8_H
#define _DALI_DT8_H

#include <stdint.h>
#include "../../config/hardware.h"

#if EVG_HAS_DT8

/* Process a DT8 extended command (224–254).
 * Called from protocol dispatcher when enabled_device_type == DALI_DEVICE_TYPE. */
void dali_dt8_process_command(uint8_t cmd);

#endif /* EVG_HAS_DT8 */
#endif /* _DALI_DT8_H */
