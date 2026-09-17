/* evse-v2-bricklet
 * Copyright (C) 2026 empunkt <empunkt@mailbox.org>
 *
 * plug_lock.c: Type 2 plug lock driven by bricklets on the ESP32
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "plug_lock.h"

#include <string.h>

#include "bricklib2/hal/system_timer/system_timer.h"
#include "bricklib2/warp/contactor_check.h"

#include "configs/config_contactor_check.h"

#include "adc.h"
#include "evse.h"
#include "hardware_version.h"
#include "iec61851.h"

PlugLock plug_lock;

// The contactor aux contacts read low while the contactor is engaged and high while it
// is released, see contactor_check.c. contactor_check itself only maintains its `state`
// field on v2 hardware and reads these pins directly on v3/v4, so we do the same rather
// than reading a field that is never updated here.
//
// invalid_counter is contactor_check's own settling window: it is raised on every
// contactor change (evse.c, dc_fault.c) and counted down again once the switching
// transient has passed. While it is non-zero neither direction is confirmed, which
// leaves the lock engaged and suppresses a spurious fault.
static bool plug_lock_contactor_settled(void) {
	return contactor_check.invalid_counter == 0;
}

// Both contactors released, so the socket cannot be live. In one phase operation the
// L2/L3 aux reads released while charging, which is why "closed" below is not simply
// the negation of this.
static bool plug_lock_contactor_confirmed_open(void) {
	return plug_lock_contactor_settled() &&
	       XMC_GPIO_GetInput(CONTACTOR_CHECK_FB1_PIN) &&
	       XMC_GPIO_GetInput(CONTACTOR_CHECK_FB2_PIN);
}

// Either contactor engaged, so the socket may be live.
static bool plug_lock_contactor_confirmed_closed(void) {
	return plug_lock_contactor_settled() &&
	       (!XMC_GPIO_GetInput(CONTACTOR_CHECK_FB1_PIN) ||
	        !XMC_GPIO_GetInput(CONTACTOR_CHECK_FB2_PIN));
}

// Deliberately derived from the CP/PE resistance and *not* from iec61851.state. A plug
// lock fault latches the state machine in IEC61851_STATE_EF for as long as the fault
// lasts, so keying the lock off the state would hold the plug shut exactly while the
// user is trying to get their cable back - and since the fault only clears when the car
// is disconnected, the two would deadlock. The resistance is the physical signal and
// answers the question we actually have: is a vehicle attached to the far end?
//
// The threshold lookup is hysteretic on the current state, which is what we want: it
// asks "would this reading take us to state A right now".
static bool plug_lock_car_detected(void) {
	// No trustworthy reading: the CP contact is disconnected, deliberately ignored or
	// forced. Do not release the plug on a guess.
	if(iec61851.force_state_f || adc_result.cp_pe_is_ignored || !evse_is_cp_connected()) {
		return true;
	}

	return adc_result.cp_pe_resistance <= iec61851_get_cp_resistance_threshold(IEC61851_STATE_A);
}

// Freshness of the ESP32's report on its own, without the dedication flag that
// plug_lock_dedication_is_currently_verified() also demands. The backstop needs this term
// alone: it asks whether anyone is listening, not whether the harness has been proven.
static bool plug_lock_report_is_fresh(void) {
	// report_time == 0 means nothing has ever been reported. Without that term the first
	// PLUG_LOCK_REPORT_STALE_MS of uptime would read as fresh - the same trap
	// plug_lock_dedication_is_currently_verified() avoids by also demanding its flag.
	return plug_lock.report_time != 0 &&
	       !system_timer_is_time_elapsed_ms(plug_lock.report_time, PLUG_LOCK_REPORT_STALE_MS);
}

void plug_lock_init(void) {
	// Save the configuration, it is persistent and read from the eeprom by
	// evse_init() before plug_lock_init() is called (same as in button_init()).
	const bool enabled_tmp = plug_lock.enabled;

	memset(&plug_lock, 0, sizeof(PlugLock));

	plug_lock.enabled = enabled_tmp;

	// Note that plug_lock_init()/plug_lock_tick() are intentionally *not* wrapped in a
	// compile-time #ifdef in main.c: activation is a runtime decision made through the
	// API. That is safe here because this module claims no pins, no CCU slices and no
	// interrupts, so a dormant instance takes nothing away from anything else.
}

void plug_lock_tick(void) {
	// The order of these two checks is deliberate and load-bearing.
	if(!hardware_version.is_v4) {
		return;
	}

	if(!plug_lock.enabled) {
		plug_lock.lock_wanted      = false;
		plug_lock.lock_wanted_time = 0;
		plug_lock.close_timed_out  = false;
		plug_lock.lock_fault       = false;
		plug_lock.report_was_fresh = false;

		// The two claims go too. Every predicate below already gates on `enabled`, so
		// leaving them set would be harmless today - but it would leave a stale claim
		// standing for whenever the feature is switched back on, and "harmless because
		// something else happens to check first" is how the stale lock_closed got in.
		plug_lock.shutdown_requested      = false;
		plug_lock.shutdown_claim_time     = 0;
		plug_lock.esp32_still_starting_up = false;
		plug_lock.startup_claim_time      = 0;

		plug_lock.state = PLUG_LOCK_STATE_DISABLED;
		return;
	}

	const bool report_fresh = plug_lock_report_is_fresh();

	// Latch the power-on window once, for plug_lock_absence_is_prolonged() to read in the
	// case where nothing has ever been reported. Everything else measures from report_time
	// and needs nothing here.
	if(!plug_lock.power_on_grace_expired &&
	   system_timer_is_time_elapsed_ms(0, PLUG_LOCK_ABSENCE_GRACE_MS)) {
		plug_lock.power_on_grace_expired = true;
	}

	// The whole of the EVSE's authority over sequencing. Locking is triggered by the
	// car being detected rather than by the contactor being requested, because the
	// release condition is then the same signal: pulling the cable at the car end
	// always gets it back. "Locked but not charging" is the intended state for
	// scheduled charging, not an anomaly.
	//
	// The release is gated on the contactor being *confirmed* open, not on it merely
	// not being requested. Commanded-open is not open: a welded contactor means the
	// socket may be live while the plug becomes removable.
	const bool lock_wanted = plug_lock_car_detected() || !plug_lock_contactor_confirmed_open();

	if(lock_wanted && !plug_lock.lock_wanted) {
		plug_lock.lock_wanted_time = system_timer_get_ms();
	} else if(!lock_wanted) {
		// Recovery is by the user disconnecting at the car end; the EVSE never retries
		// on its own. Retry policy belongs with the actuation logic on the ESP32.
		plug_lock.lock_wanted_time = 0;
		plug_lock.close_timed_out  = false;
		plug_lock.lock_fault       = false;
	} else if(report_fresh && !plug_lock.report_was_fresh) {
		// The ESP32 has just reappeared while the plug is still held. Restart the window
		// from here: it had no opportunity to act while it was away, and its first report
		// after booting carries lock_closed = false until its feedback input debounces.
		// Without this the backstop expires unobserved during the gap and latches on that
		// first report, turning a transient loss of the assertion into a fault that only
		// a disconnect at the car end clears. See PLUG_LOCK_CLOSE_TIMEOUT_MS.
		plug_lock.lock_wanted_time = system_timer_get_ms();
	}

	plug_lock.lock_wanted      = lock_wanted;
	plug_lock.report_was_fresh = report_fresh;

	// Gated on report_fresh for the mirror of the case above: an ESP32 that goes away
	// mid-attempt, with lock_closed still false, would otherwise latch the backstop
	// during the gap and keep it latched through its return.
	//
	// The gap is not unguarded, but note what guards it, because it is no longer this:
	// plug_lock_has_fault() used to fault on any silence past PLUG_LOCK_REPORT_STALE_MS
	// and the absence grace removed that deliberately. What holds now is
	// plug_lock_contactor_close_is_blocked(), which refuses on a stale lock_closed and
	// therefore covers the whole absence on its own.
	if(report_fresh && lock_wanted && !plug_lock.lock_closed && !plug_lock.close_timed_out &&
	   system_timer_is_time_elapsed_ms(plug_lock.lock_wanted_time, PLUG_LOCK_CLOSE_TIMEOUT_MS)) {
		plug_lock.close_timed_out = true;
	}

	if(!plug_lock_dedication_is_currently_verified()) {
		plug_lock.state = PLUG_LOCK_STATE_BRICKLET_DEDICATION_NOT_VERIFIED;
	} else if(plug_lock.close_timed_out) {
		plug_lock.state = PLUG_LOCK_STATE_FAULT_TIMEOUT;
	} else if(plug_lock.lock_fault) {
		plug_lock.state = PLUG_LOCK_STATE_FAULT_LOCK;
	} else if(!lock_wanted) {
		plug_lock.state = PLUG_LOCK_STATE_IDLE;
	} else if(plug_lock.lock_closed) {
		plug_lock.state = PLUG_LOCK_STATE_LOCKED;
	} else {
		plug_lock.state = PLUG_LOCK_STATE_WAITING;
	}
}

bool plug_lock_dedication_is_currently_verified(void) {
	// bricklet_dedication_verified starts out false and is only ever set together with
	// report_time, so a report_time of 0 can never be mistaken for a fresh assertion
	// during the first PLUG_LOCK_REPORT_STALE_MS after boot.
	return plug_lock.bricklet_dedication_verified &&
	       !system_timer_is_time_elapsed_ms(plug_lock.report_time, PLUG_LOCK_REPORT_STALE_MS);
}

// Whether an absence has lasted long enough to be a failure rather than a restart.
// Deliberately about *duration only*: what a boot, a reboot and a dead ESP32 have in
// common is silence, and the sole thing that tells them apart is how long it lasts.
static bool plug_lock_absence_is_prolonged(void) {
	// Nothing has ever been reported, so there is no last-heard-from moment and the window
	// runs from power-on instead. The latch is what keeps that correct across the
	// millisecond wraparound; comparing against 0 here would re-open it every 49.7 days.
	if(plug_lock.report_time == 0) {
		return plug_lock.power_on_grace_expired;
	}

	return system_timer_is_time_elapsed_ms(plug_lock.report_time, PLUG_LOCK_ABSENCE_GRACE_MS);
}

// Whether to still believe the ESP32's claim that it has not settled yet. Bounded on
// purpose: the claim is the one thing here the ESP32 asserts that makes this charger
// *less* strict, so it gets a deadline rather than trust. An ESP32 that never stops
// claiming it is starting up faults at the same moment one that says nothing does.
static bool plug_lock_startup_claim_holds(void) {
	return plug_lock.esp32_still_starting_up &&
	       !system_timer_is_time_elapsed_ms(plug_lock.startup_claim_time, PLUG_LOCK_CLAIM_TIMEOUT_MS);
}

bool plug_lock_shutdown_is_requested(void) {
	if(!hardware_version.is_v4 || !plug_lock.enabled || !plug_lock.shutdown_requested) {
		return false;
	}

	// Bounded only while the ESP32 is still here to be contradicted. Once its reports stop
	// the claim keeps standing, which is the whole point of it: it has to cover the
	// absence it announced, and that absence is exactly when nothing can renew it.
	//
	// What does get bounded is the contradiction. An ESP32 that is alive, heartbeating and
	// still announcing a restart thirty seconds later is not restarting, and must not be
	// able to hold a charger off the grid indefinitely by saying so. Letting the gate go
	// restores service; the veto and the fault logic are untouched and still apply.
	return !plug_lock_report_is_fresh() ||
	       !system_timer_is_time_elapsed_ms(plug_lock.shutdown_claim_time, PLUG_LOCK_CLAIM_TIMEOUT_MS);
}

bool plug_lock_has_fault(void) {
	// Gated on `enabled` alone, deliberately NOT on "enabled and the dedication is
	// verified". Adding the dedication term here would mean that losing the hardware
	// makes the check pass, which is exactly inverted - it is the moment the fault has
	// to fire. The same trap applies to plug_lock_contactor_close_is_blocked(); do not "tidy" the
	// two into a shared is-this-thing-working predicate.
	if(!hardware_version.is_v4 || !plug_lock.enabled) {
		return false;
	}

	// The first term separates two things that a bare "not verified" runs together.
	//
	//   report_fresh, settled     - the ESP32 is here, has had its chance to reach a
	//                               verdict, and is telling us the dedication does not
	//                               hold. That is a reported problem: a pulled Bricklet,
	//                               a broken harness loop, a dead lock supply. It faults
	//                               at once. The "settled" half is what keeps the first
	//                               seconds of a start-up out of this branch, where a
	//                               plain false means "not yet" rather than "broken".
	//   !contactor_confirmed_open - the socket may be live. Waiting out a grace over a
	//                               running session would mean a minute of energized
	//                               contacts with nobody watching the lock, so silence
	//                               gets no grace at all here: it faults on the ordinary
	//                               PLUG_LOCK_REPORT_STALE_MS. A deliberate restart does
	//                               not land here, because the shutdown gate has already
	//                               taken the session to IEC61851_STATE_B; a crash does,
	//                               and must.
	//   absence_is_prolonged      - nobody is telling us anything, and it has gone on
	//                               longer than a boot or a restart plausibly takes.
	//
	// A short silence with the contactor open is none of the three, and that is the case
	// this qualification exists for. Energizing is refused throughout regardless - that is
	// plug_lock_contactor_close_is_blocked(), which refuses while lock_closed is stale and
	// so covers exactly the window these graces open. The two were changed together and
	// depend on each other: without the freshness term over there, a grace here would let
	// a stale "locked" stand as permission. What the graces remove is the *claim that
	// something is wrong*, never the block.
	//
	// Note that plug_lock.shutdown_requested is deliberately absent from this expression.
	// A message from the ESP32 may make this charger more restrictive and never less, so
	// the fault is bounded by measured silence rather than by anything the ESP32 asserts.
	//
	// The fourth term is the one the veto cannot catch, and it is outside every grace:
	// the veto only ever guards a request to close the contactor, so it cannot drop a
	// contactor that is already closed. Feedback missing while the socket is live means
	// the plug may have come unlocked, which is the exact hazard this whole module exists
	// to prevent.
	return (!plug_lock_dedication_is_currently_verified() &&
	        ((plug_lock_report_is_fresh() && !plug_lock_startup_claim_holds()) ||
	         !plug_lock_contactor_confirmed_open() ||
	         plug_lock_absence_is_prolonged())) ||
	       plug_lock.close_timed_out ||
	       plug_lock.lock_fault ||
	       (plug_lock_contactor_confirmed_closed() && !plug_lock.lock_closed);
}

bool plug_lock_contactor_close_is_blocked(void) {
	// The freshness term is what makes this an independent layer rather than a second
	// reading of the same stored byte. plug_lock.lock_closed keeps its last asserted value
	// for as long as nobody overwrites it, so without it a report that stopped arriving
	// would leave a stale "locked" standing as permission to energize - which is the exact
	// opposite of what plug_lock.h promises above.
	//
	// It was latent while plug_lock_has_fault() faulted on any silence past
	// PLUG_LOCK_REPORT_STALE_MS, because that dropped the contactor before the stale value
	// could be acted on. The absence grace removed that cover deliberately, so this has to
	// stand on its own now.
	return hardware_version.is_v4 && plug_lock.enabled &&
	       !(plug_lock.lock_closed && plug_lock_report_is_fresh());
}

bool plug_lock_bricklets_are_currently_not_found(void) {
	// Same freshness rule as plug_lock_dedication_is_currently_verified(), and for the
	// same reason: a stale report is not evidence of anything. Note the consequence -
	// with no report at all this is false, so the disable stays blocked. Silence must
	// never be read as permission.
	return plug_lock.bricklets_not_found &&
	       !system_timer_is_time_elapsed_ms(plug_lock.report_time, PLUG_LOCK_REPORT_STALE_MS);
}

bool plug_lock_disable_is_blocked(void) {
	// See the comment in plug_lock.h: the gate is the hardware being demonstrably gone,
	// not the harness loop and not the session. Deliberately does not consult the CP
	// state either, so a disconnected CP - which is routine - cannot wedge the feature
	// into a state that only a power cycle clears.
	return hardware_version.is_v4 && plug_lock.enabled &&
	       !plug_lock_bricklets_are_currently_not_found();
}

bool plug_lock_enable_is_blocked(void) {
	// Deliberately *not* gated on plug_lock.enabled: this guards the transition into
	// enabled, so it has to answer while the feature is still off.
	//
	// Enabling while the contactor is closed would stop the running charge on the very
	// next tick. plug_lock_has_fault() treats "contactor confirmed closed and the lock
	// not confirmed closed" as the plug having come unlocked mid-session, and the lock
	// cannot possibly be confirmed closed at the instant the feature is switched on -
	// nothing has asked the ESP32 to lock anything yet. The user would flip a setting
	// and have their session dropped into IEC61851_STATE_EF, which then needs a
	// disconnect to clear.
	//
	// A car connected with the contactor open is fine and deliberately still allowed:
	// lock_wanted goes true, the ESP32 locks within its budget, and the fault term
	// cannot fire because the contactor is open.
	return hardware_version.is_v4 && !plug_lock_contactor_confirmed_open();
}

PlugLockState plug_lock_get_state(void) {
	return plug_lock.state;
}
