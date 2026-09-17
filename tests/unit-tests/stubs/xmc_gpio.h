/* Host test stub for xmc_gpio.h
 *
 * Two jobs:
 *
 * 1. Give the *real* hardware_version.h the XMC_GPIO_PORT_t type name it needs for
 *    its port/pin accessor declarations. Shadowing xmc_gpio.h rather than
 *    hardware_version.h keeps the test compiling against the actual project header.
 * 2. Let the tests drive the contactor feedback pins that plug_lock.c reads through
 *    the real configs/config_contactor_check.h.
 *
 * The pin macros follow the real xmclib form, xmclib/XMCLib/inc/xmc4_gpio_map.h:105:
 * P2_6 expands to two arguments, `XMC_GPIO_PORT2, 6`.
 */

#ifndef XMC_GPIO_H
#define XMC_GPIO_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	int unused;
} XMC_GPIO_PORT_t;

#define STUB_GPIO_PORTS 16
#define STUB_GPIO_PINS  16

// Port handles are only ever compared and indexed here, never dereferenced, so a
// small integer cast to the port type is enough to tell them apart.
#define XMC_GPIO_PORT2 ((XMC_GPIO_PORT_t *)2)

#define P2_6 XMC_GPIO_PORT2, 6
#define P2_7 XMC_GPIO_PORT2, 7
#define P2_8 XMC_GPIO_PORT2, 8

// Defined by the test binary. Pins read low (0) unless a test says otherwise, which
// on this board means "contactor engaged" - the conservative default.
extern bool stub_gpio_input[STUB_GPIO_PORTS][STUB_GPIO_PINS];

static inline uint32_t XMC_GPIO_GetInput(XMC_GPIO_PORT_t *const port, const uint8_t pin) {
	return stub_gpio_input[(uintptr_t)port % STUB_GPIO_PORTS][pin % STUB_GPIO_PINS] ? 1u : 0u;
}

#endif
