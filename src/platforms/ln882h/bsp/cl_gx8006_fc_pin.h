#pragma once

#ifdef CONFIG_IOT_PLATFORM_LN882H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GX8006 flow-control pin. Encoding follows the SDK's pin numbering:
// 0..15 = GPIOA pins, 16..31 = GPIOB pins. HIGH means the chip can accept
// speaker data (docs/ln882h.md §3.1). Verify the pin against the board.
void gx8006_fc_pin_init(uint16_t pin_encoding);
bool gx8006_fc_pin_read(uint16_t pin_encoding);

#ifdef __cplusplus
}
#endif

#endif  // CONFIG_IOT_PLATFORM_LN882H
