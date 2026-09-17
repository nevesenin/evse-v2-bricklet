/* Forced first include for the host tests, see the Makefile.
 *
 * -Istubs alone is not enough for anything under bricklib2/. A quoted include is
 * resolved relative to the *including file* before any -I directory is consulted,
 * and software/src/bricklib2 is a symlink into the bricklib2 checkout that the
 * firmware build creates. So once you have built the firmware once,
 * `#include "bricklib2/hal/system_timer/system_timer.h"` in plug_lock.c finds the
 * real header, which pulls in configs/config.h and then xmc_device.h, and the test
 * build stops. Whether the tests compiled therefore depended on whether a firmware
 * build had run, which is not a property worth having.
 *
 * Pulling the stubs in first fixes that: they use the same include guards as the
 * headers they stand in for, so the later real include expands to nothing. The
 * stubs are found here because a quoted include inside this file resolves relative
 * to this file, i.e. to stubs/.
 *
 * This only shadows the hardware layer. plug_lock.c still compiles against the real
 * plug_lock.h, hardware_version.h, adc.h, evse.h, iec61851.h and
 * configs/config_contactor_check.h.
 */

#ifndef FORCE_STUBS_H
#define FORCE_STUBS_H

#include "xmc_gpio.h"
#include "xmc_vadc.h"
#include "bricklib2/hal/system_timer/system_timer.h"
#include "bricklib2/warp/contactor_check.h"

#endif
