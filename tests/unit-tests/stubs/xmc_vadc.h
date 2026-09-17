/* Host test stub for xmc_vadc.h
 *
 * plug_lock.c reads adc_result out of the real adc.h, which pulls this in for the
 * VADC types used by parts of that header the tests never touch. Nothing here is
 * dereferenced, so opaque structs are enough.
 */

#ifndef XMC_VADC_H
#define XMC_VADC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	int unused;
} XMC_VADC_GROUP_t;

typedef struct {
	int unused;
} XMC_VADC_GLOBAL_t;

#endif
