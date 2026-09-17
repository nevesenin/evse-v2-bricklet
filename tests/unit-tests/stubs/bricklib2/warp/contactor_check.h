/* Host test stub for bricklib2/warp/contactor_check.h
 *
 * The real header lives in the bricklib2 checkout, which is reached through a
 * symlink that these tests deliberately do not require. plug_lock.c only reads
 * invalid_counter, so that is all this carries; keep it in sync if that changes.
 */

#ifndef CONTACTOR_CHECK_H
#define CONTACTOR_CHECK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint8_t invalid_counter;
} ContactorCheck;

// Defined by the test binary.
extern ContactorCheck contactor_check;

#endif
