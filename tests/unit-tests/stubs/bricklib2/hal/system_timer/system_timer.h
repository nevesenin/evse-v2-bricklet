/* Host test stub for bricklib2/hal/system_timer/system_timer.h
 *
 * The real header pulls in configs/config.h and the real implementation reads an
 * XMC timer, neither of which exists on the host. Here the clock is simply a
 * variable that the tests move by hand.
 */

#ifndef SYSTEM_TIMER_H
#define SYSTEM_TIMER_H

#include <stdint.h>
#include <stdbool.h>

// Defined by the test binary.
extern uint32_t stub_now_ms;

static inline uint32_t system_timer_get_ms(void) {
	return stub_now_ms;
}

// Copied verbatim from bricklib2/hal/system_timer/system_timer.c:109 so the tests
// exercise the real unsigned-wraparound arithmetic rather than an approximation
// of it. The cast is what makes the comparison survive the 32 bit millisecond
// counter wrapping, which happens every ~49.7 days of uptime.
static inline bool system_timer_is_time_elapsed_ms(const uint32_t start_measurement,
                                                   const uint32_t time_to_be_elapsed) {
	return (uint32_t)(system_timer_get_ms() - start_measurement) >= time_to_be_elapsed;
}

#endif
