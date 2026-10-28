#ifdef CONFIG_IOT_PLATFORM_LN882H

#include "cl_gx8006_fc_pin.h"

#include "hal/hal_gpio.h"

void gx8006_fc_pin_init(uint16_t pin_encoding) {
  const uint32_t port_base = (pin_encoding >= 16) ? GPIOB_BASE : GPIOA_BASE;
  const gpio_pin_t pin = (gpio_pin_t)(1u << (pin_encoding & 0x0F));
  // The SDK's zero-initialized gpio_init_t_def defaults to input with the
  // reference demo's pull setting; only the pin is required.
  gpio_init_t_def init = {0};
  init.pin = pin;
  hal_gpio_init(port_base, &init);
}

bool gx8006_fc_pin_read(uint16_t pin_encoding) {
  const uint32_t port_base = (pin_encoding >= 16) ? GPIOB_BASE : GPIOA_BASE;
  const gpio_pin_t pin = (gpio_pin_t)(1u << (pin_encoding & 0x0F));
  return hal_gpio_pin_read(port_base, pin) != 0;
}

#endif  // CONFIG_IOT_PLATFORM_LN882H
