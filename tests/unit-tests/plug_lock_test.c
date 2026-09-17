/* evse-v2-bricklet
 * Copyright (C) 2026 empunkt <empunkt@mailbox.org>
 *
 * plug_lock_test.c: Host unit tests for plug_lock.c
 *
 * plug_lock.c claims no pins, no CCU slices and no interrupts, so its whole
 * outside world is hardware_version.is_v4 and system_timer_is_time_elapsed_ms().
 * That makes it testable on the host against two small stubs; see stubs/.
 *
 * WHAT THESE TESTS CANNOT REACH. Only plug_lock.c is compiled here, so every
 * assertion below is about what the module *answers*, never about what its
 * callers then do. The two consumers live outside that boundary:
 *
 *   plug_lock_has_fault()               -> iec61851.c, IEC61851_STATE_EF
 *   plug_lock_contactor_close_is_blocked() -> evse.c, evse_set_output()
 *   plug_lock_shutdown_is_requested()   -> iec61851.c, STATE_B over STATE_C
 *
 * The third is the least covered of the three: nothing here demonstrates that a
 * shutdown request actually ends a session, only that the predicate goes true.
 * That property is verified by the guided hardware test, not from here.
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

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "plug_lock.h"
#include "hardware_version.h"
#include "adc.h"
#include "evse.h"
#include "iec61851.h"
#include "configs/config_contactor_check.h"

// Normally provided by hardware_version.c and the system timer, neither of which
// is compiled here.
HardwareVersion hardware_version;
uint32_t stub_now_ms;

// The inputs plug_lock.c senses. Real headers, fake storage: the module is linked
// against these instead of against the modules that own them on the target.
ContactorCheck contactor_check;
IEC61851 iec61851;
ADCResult adc_result;
bool stub_gpio_input[STUB_GPIO_PORTS][STUB_GPIO_PINS];

static bool stub_cp_connected;

bool evse_is_cp_connected(void) {
	return stub_cp_connected;
}

// Mirrors the "transition to state A" column of cp_resistance_state in iec61851.c,
// including its hysteresis, so that the tests exercise the same state dependence the
// firmware has. Getting this wrong in the fake would hide exactly the case that
// matters: a plug lock fault latches iec61851.state at EF, and the module must still
// have to see the car go away.
uint16_t iec61851_get_cp_resistance_threshold(IEC61851State transition_to_state) {
	(void)transition_to_state; // plug_lock.c only ever asks about state A

	switch(iec61851.state) {
		case IEC61851_STATE_A:  return 9000;
		case IEC61851_STATE_B:  return 11000;
		default:                return 10000;
	}
}

// The contactor feedback contacts read low while the contactor is engaged.
static void given_contactor_open(void) {
	contactor_check.invalid_counter = 0;
	stub_gpio_input[2][7]           = true; // CONTACTOR_CHECK_FB1_PIN, P2_7
	stub_gpio_input[2][6]           = true; // CONTACTOR_CHECK_FB2_PIN, P2_6
}

static void given_contactor_closed(void) {
	contactor_check.invalid_counter = 0;
	stub_gpio_input[2][7]           = false;
	stub_gpio_input[2][6]           = false;
}

// 10 kOhm CP/PE is the nominal "nothing attached" reading; anything at or below the
// state A threshold means a vehicle is on the far end.
static void given_no_car(void) {
	stub_cp_connected            = true;
	iec61851.state               = IEC61851_STATE_A;
	iec61851.force_state_f       = false;
	adc_result.cp_pe_is_ignored  = false;
	adc_result.cp_pe_resistance  = 100000;
}

static void given_car_connected(void) {
	given_no_car();
	iec61851.state              = IEC61851_STATE_B;
	adc_result.cp_pe_resistance = 2700; // the usual state B resistance
}

// Exactly what set_plug_lock_hardware_state() does with shutting_down = true, edge stamp
// included. Setting the flag on its own is not a state the firmware can reach, so a test
// that does it by hand would be asserting about a shape the handler never produces.
static void given_shutdown_announced(void) {
	if(!plug_lock.shutdown_requested) {
		plug_lock.shutdown_claim_time = stub_now_ms;
	}

	plug_lock.shutdown_requested = true;
}

static int checks;
static int failures;

// The runner prints the test name without a trailing newline so the result can be
// appended to it. The first failing check inside a test has to break that line
// before it can report, hence this flag.
static bool name_line_open;

#define CHECK(cond)                                                     \
	do {                                                                \
		checks++;                                                       \
		if(!(cond)) {                                                   \
			if(name_line_open) {                                        \
				printf("\n");                                           \
				name_line_open = false;                                 \
			}                                                           \
			printf("      line %d: %s\n", __LINE__, #cond);             \
			failures++;                                                 \
		}                                                               \
	} while(false)

// Put the module into "v4, enabled, hardware asserted just now, nothing plugged in
// and the contactor confirmed open" and let each test move away from that.
static void given_operable(void) {
	memset(&hardware_version, 0, sizeof(hardware_version));
	hardware_version.is_v4 = true;

	stub_now_ms = 100000;

	memset(&contactor_check, 0, sizeof(contactor_check));
	memset(&iec61851, 0, sizeof(iec61851));
	memset(&adc_result, 0, sizeof(adc_result));
	memset(stub_gpio_input, 0, sizeof(stub_gpio_input));

	given_contactor_open();
	given_no_car();

	memset(&plug_lock, 0, sizeof(plug_lock));
	plug_lock.enabled                      = true;
	plug_lock.bricklet_dedication_verified = true;
	plug_lock.report_time                  = stub_now_ms;
}

static void test_idle_when_enabled_and_fresh(void) {
	given_operable();
	plug_lock_tick();

	CHECK(plug_lock_dedication_is_currently_verified());
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_IDLE);
}

// "If not enabled, do nothing": a disabled module must be inert whatever the
// hardware is doing, and must never contribute a fault.
static void test_disabled_is_inert(void) {
	given_operable();
	plug_lock.enabled = false;
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_DISABLED);

	// Same with the hardware gone: still no fault, because it was never enabled.
	plug_lock.bricklet_dedication_verified = false;
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_DISABLED);
}

// The feature is v4 only. On v2/v3 nothing may fire even if the config page
// somehow carries enabled = true, e.g. after an EVSE board swap.
static void test_not_v4_is_inert(void) {
	given_operable();
	hardware_version.is_v4                 = false;
	hardware_version.is_v3                 = true;
	plug_lock.bricklet_dedication_verified = false;
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());
}

// Everything a cold boot needs, with nothing ever reported and the contactor released.
static void given_cold_boot(void) {
	memset(&hardware_version, 0, sizeof(hardware_version));
	hardware_version.is_v4 = true;

	memset(&contactor_check, 0, sizeof(contactor_check));
	memset(&iec61851, 0, sizeof(iec61851));
	memset(&adc_result, 0, sizeof(adc_result));
	memset(stub_gpio_input, 0, sizeof(stub_gpio_input));

	given_contactor_open();
	given_no_car();

	stub_now_ms = 12; // 12 ms of uptime, well inside the staleness window

	memset(&plug_lock, 0, sizeof(plug_lock));
	plug_lock.enabled = true; // restored from eeprom by evse_init()
}

// Fail-closed at boot: enabled is restored from the config page and no assertion has
// arrived yet, so charging must be blocked. It must NOT also be called a fault - a boot
// is not a failure, and sitting in IEC61851_STATE_EF blinking an error code for the first
// seconds of every power cycle says nothing true. The block is what the principle
// requires; the fault is a claim about the world that there is no evidence for yet.
//
// This is also the case that makes report_time == 0 dangerous, since "now - 0" is small
// right after boot.
static void test_boot_before_first_assertion_blocks_without_faulting(void) {
	given_cold_boot();
	plug_lock_tick();

	CHECK(!plug_lock_dedication_is_currently_verified());
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_contactor_close_is_blocked()); // the part that matters
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_BRICKLET_DEDICATION_NOT_VERIFIED);

	// One millisecond short of the window: still not a fault.
	stub_now_ms = PLUG_LOCK_ABSENCE_GRACE_MS - 1;
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_contactor_close_is_blocked());

	// Bounded, and it expires into the fault. An ESP32 that never arrives at all is
	// still reported, just 30 s later instead of immediately.
	stub_now_ms = PLUG_LOCK_ABSENCE_GRACE_MS;
	plug_lock_tick();

	CHECK(plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_BRICKLET_DEDICATION_NOT_VERIFIED);
}

// Once something has been reported, the window is measured from that report rather than
// from power-on. The two only ever differ in what there is to subtract from, but getting
// it wrong either way would mis-time every fault on a charger that has been up a while.
static void test_absence_window_runs_from_the_last_report(void) {
	given_cold_boot();
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());

	// The ESP32 arrives well inside the window.
	stub_now_ms = 4500;
	plug_lock.bricklet_dedication_verified = true;
	plug_lock.report_time                  = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_IDLE);

	// ... and goes away again. The window is measured from the last report, not from
	// power-on: at 61 s of uptime the power-on latch has expired, but only 56.5 s of
	// silence have passed, so it does not govern and there is no fault.
	stub_now_ms = PLUG_LOCK_ABSENCE_GRACE_MS + 1000;
	plug_lock_tick();

	CHECK(plug_lock.power_on_grace_expired);
	CHECK(!plug_lock_dedication_is_currently_verified());
	CHECK(!plug_lock_has_fault());

	// And it does expire on its own clock, so an ESP32 that dies right after boot is
	// still reported - it just is not called dead at the five second mark.
	stub_now_ms = 4500 + PLUG_LOCK_ABSENCE_GRACE_MS;
	plug_lock_tick();

	CHECK(plug_lock_has_fault());
}

// The case the start-up grace was written for and, for one revision, did not actually cover:
// a real start-up is not silent. The ESP32 comes up at about two seconds and reports long
// before its harness loop has a verdict, so bricklet_dedication_verified is false in a
// report that is perfectly fresh. Without still_starting_up that is indistinguishable
// from "checked and broken" and faults immediately, which is what every boot looked like.
static void test_startup_claim_covers_an_early_unverified_report(void) {
	given_cold_boot();
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());

	// Up and reporting, no verdict yet.
	stub_now_ms                       = 2000;
	plug_lock.esp32_still_starting_up = true;
	plug_lock.startup_claim_time      = stub_now_ms;
	plug_lock.report_time             = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock_dedication_is_currently_verified());
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_contactor_close_is_blocked()); // blocked throughout, as always

	// The verdict lands and it is good.
	stub_now_ms                            = 4400;
	plug_lock.esp32_still_starting_up      = false;
	plug_lock.bricklet_dedication_verified = true;
	plug_lock.report_time                  = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_IDLE);
}

// The other half: once the ESP32 says it has settled, an unverified dedication is a
// reported problem and faults at once. No grace, because this is not a start-up.
static void test_settled_and_unverified_faults_at_once(void) {
	given_cold_boot();
	stub_now_ms                            = 2000;
	plug_lock.esp32_still_starting_up      = false; // settled: it checked
	plug_lock.bricklet_dedication_verified = false; // and it is broken
	plug_lock.report_time                  = stub_now_ms;
	plug_lock_tick();

	CHECK(plug_lock_has_fault());
}

// The claim is the one assertion here that makes this charger *less* strict, so it is
// bounded rather than trusted. An ESP32 that never stops claiming to be starting up
// faults at the same moment one that says nothing at all does.
static void test_startup_claim_is_bounded(void) {
	given_cold_boot();

	stub_now_ms                       = 2000;
	plug_lock.esp32_still_starting_up = true;
	plug_lock.startup_claim_time      = stub_now_ms;
	plug_lock.report_time             = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());

	// Still claiming, still heartbeating, one millisecond short of the bound.
	stub_now_ms           = 2000 + PLUG_LOCK_CLAIM_TIMEOUT_MS - 1;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());

	stub_now_ms           = 2000 + PLUG_LOCK_CLAIM_TIMEOUT_MS;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(plug_lock_has_fault());
}

// The veto has to refuse on a *stale* assertion, not merely on an absent one. lock_closed
// keeps its last asserted value indefinitely, so an ESP32 that vanishes while the plug is
// locked leaves a standing "yes" behind it. This was latent for as long as any silence
// past PLUG_LOCK_REPORT_STALE_MS faulted and dropped the contactor anyway; the absence
// grace removes that cover on purpose, so the veto now has to hold this on its own.
static void test_veto_refuses_a_stale_lock_closed(void) {
	given_operable();
	given_car_connected();
	plug_lock.lock_closed = true;
	plug_lock_tick();

	CHECK(!plug_lock_contactor_close_is_blocked());

	// The ESP32 goes away with lock_closed still standing at true. Inside the absence
	// grace, so plug_lock_has_fault() is deliberately silent - and the veto is all there
	// is between a stale byte and a closed contactor.
	stub_now_ms += PLUG_LOCK_REPORT_STALE_MS;
	plug_lock_tick();

	CHECK(plug_lock.lock_closed); // nothing cleared it, and nothing should
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_contactor_close_is_blocked());

	// A fresh report restores permission without anything else having to happen.
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock_contactor_close_is_blocked());
}

// The ESP32 announcing a deliberate restart interrupts charging, and does it through the
// ordinary route: iec61851_tick() reads this and selects state B over state C, releasing
// the contactor with the plug still connected and the PWM still running.
//
// Interrupts, not ends. Nothing here records that a session was stopped, so once the
// returning instance clears the flag and CP is still state C, charging resumes on its own.
// What the announcement guarantees is that the contactor is open for the duration of the
// absence - not that the session is over.
static void test_shutdown_request_is_reported(void) {
	given_operable();
	given_car_connected();
	plug_lock_tick();

	CHECK(!plug_lock_shutdown_is_requested());

	given_shutdown_announced(); // as FID_SET_PLUG_LOCK_HARDWARE_STATE does
	plug_lock_tick();

	CHECK(plug_lock_shutdown_is_requested());

	// It has to survive the absence it announced, so going stale must not release it -
	// that is exactly the window it exists to cover.
	stub_now_ms += PLUG_LOCK_REPORT_STALE_MS + 1;
	plug_lock_tick();

	CHECK(plug_lock_shutdown_is_requested());

	// The instance that comes back clears it by asserting false. No user action and no
	// reboot of this board is needed to release it.
	plug_lock.shutdown_requested = false;
	plug_lock.report_time        = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock_shutdown_is_requested());
}

// The claim is bounded against the ESP32 contradicting itself: one that is alive,
// heartbeating and still announcing a restart is not restarting, and must not be able to
// hold a charger off the grid by saying so forever. Note the asymmetry with the case
// above - silence keeps the claim, a live contradiction ends it.
static void test_shutdown_claim_is_bounded_while_the_esp32_is_alive(void) {
	given_operable();
	given_car_connected();
	plug_lock.lock_closed = true; // a locked session, so the backstop is not in play
	given_shutdown_announced();
	plug_lock_tick();

	CHECK(plug_lock_shutdown_is_requested());

	// Still claiming, still heartbeating, one millisecond short of the bound.
	stub_now_ms          += PLUG_LOCK_CLAIM_TIMEOUT_MS - 1;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(plug_lock_shutdown_is_requested());

	stub_now_ms          += 1;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock_shutdown_is_requested());

	// Releasing the gate restores service; it is not a fault, and nothing else moved.
	CHECK(!plug_lock_has_fault());
}

// It says nothing while the feature is off or on the wrong hardware, for the same reason
// every other predicate here does: there is no lock to interrupt charging over.
static void test_shutdown_request_is_inert_when_disabled(void) {
	given_operable();
	given_shutdown_announced();
	plug_lock.enabled = false;
	plug_lock_tick();

	CHECK(!plug_lock_shutdown_is_requested());

	given_operable();
	given_shutdown_announced();
	hardware_version.is_v4 = false;
	plug_lock_tick();

	CHECK(!plug_lock_shutdown_is_requested());
}

// The announcement may make this charger more restrictive and never less. It ends the
// session; it must not also buy silence about a genuine problem, or a caller able to
// reach FID_SET_PLUG_LOCK_HARDWARE_STATE could suppress the fault by claiming to restart.
static void test_shutdown_request_never_suppresses_a_fault(void) {
	given_operable();
	given_car_connected();
	given_shutdown_announced();

	// A reported problem still faults at once, announcement or not.
	plug_lock.bricklet_dedication_verified = false;
	plug_lock.report_time                  = stub_now_ms;
	plug_lock_tick();

	CHECK(plug_lock_shutdown_is_requested());
	CHECK(plug_lock_has_fault());

	// And so does a live socket with no assertion that the plug is locked.
	given_operable();
	given_car_connected();
	given_contactor_closed();
	given_shutdown_announced();
	plug_lock.lock_closed = false;
	plug_lock_tick();

	CHECK(plug_lock_has_fault());
}

// The grace removes the claim that something is wrong; it does not remove the hazard
// check. A contactor that is already closed when this board boots, with no assertion that
// the plug is locked, is the mid-session hazard and has to fire at once - the veto cannot
// catch it, because the veto only guards a request to close.
static void test_absence_grace_does_not_cover_a_closed_contactor(void) {
	given_cold_boot();
	given_contactor_closed();
	plug_lock_tick();

	CHECK(!plug_lock_dedication_is_currently_verified());
	CHECK(plug_lock_has_fault());
}

static void test_assertion_goes_stale(void) {
	given_operable();

	// One millisecond short of the window: still fresh.
	stub_now_ms += PLUG_LOCK_REPORT_STALE_MS - 1;
	plug_lock_tick();

	CHECK(plug_lock_dedication_is_currently_verified());
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_IDLE);

	// Exactly at the window: elapsed, because the comparison is >=.
	stub_now_ms += 1;
	plug_lock_tick();

	CHECK(!plug_lock_dedication_is_currently_verified());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_BRICKLET_DEDICATION_NOT_VERIFIED);

	// Staleness ends the *verification* at five seconds and always has. What it no longer
	// does by itself is raise a fault: with the contactor open there is nothing to
	// protect and a restart looks exactly like this, so the absence grace governs.
	CHECK(!plug_lock_has_fault());

	// With the socket possibly live it gets no grace whatsoever. This is the crash case -
	// a deliberate restart never reaches it, because the shutdown gate has already taken
	// the session to IEC61851_STATE_B.
	given_contactor_closed();
	plug_lock_tick();

	CHECK(plug_lock_has_fault());
}

// A fresh assertion after a stale one must recover, so that unplugging and
// replugging a bricklet clears the fault without a reboot.
static void test_fresh_assertion_recovers(void) {
	given_operable();
	stub_now_ms += PLUG_LOCK_ABSENCE_GRACE_MS;
	plug_lock_tick();
	CHECK(plug_lock_has_fault());

	plug_lock.report_time = stub_now_ms; // as FID_SET_PLUG_LOCK_HARDWARE_STATE does
	plug_lock_tick();

	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_IDLE);
}

// The millisecond counter wraps roughly every 49.7 days and a charger runs
// continuously, so this will happen in the field. The unsigned subtraction must
// carry the freshness check across the wrap rather than reporting a huge age.
static void test_survives_millisecond_counter_wraparound(void) {
	given_operable();

	plug_lock.report_time = UINT32_MAX - 1000; // assertion just before the wrap
	stub_now_ms           = 1000;              // 2000 ms later, counter has wrapped

	plug_lock_tick();

	CHECK(plug_lock_dedication_is_currently_verified());
	CHECK(!plug_lock_has_fault());

	// And it still expires correctly on the far side of the wrap.
	stub_now_ms = UINT32_MAX - 1000 + PLUG_LOCK_REPORT_STALE_MS;
	plug_lock_tick();

	CHECK(!plug_lock_dedication_is_currently_verified());

	// The absence grace is the same unsigned comparison against the same timestamp, so it
	// has to survive the wrap too - and it is the one that decides whether a charger three
	// weeks into an uptime silently stops reporting a dead ESP32.
	stub_now_ms = UINT32_MAX - 1000 + PLUG_LOCK_ABSENCE_GRACE_MS;
	plug_lock_tick();

	CHECK(plug_lock_has_fault());

	// Well past the deadline rather than exactly on it. This case earns its keep:
	// at exactly report_time + STALE_MS the freshness comparison happens to be
	// symmetric in its two arguments, so a boundary-only test cannot tell a
	// correct system_timer_is_time_elapsed_ms(report_time, STALE_MS) from a
	// transposed ...(STALE_MS, report_time). The transposed form reports a
	// long-stale assertion as fresh, which fails open.
	stub_now_ms = UINT32_MAX - 1000 + (PLUG_LOCK_ABSENCE_GRACE_MS * 2);
	plug_lock_tick();

	CHECK(!plug_lock_dedication_is_currently_verified());
	CHECK(plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_BRICKLET_DEDICATION_NOT_VERIFIED);
}

// Regression guard for a recorded trap: the fault must gate on `enabled`, never on
// "enabled and the dedication is verified". That composite is false exactly when the
// hardware has gone away, which is the moment the fault has to fire, so gating on it
// inverts the check.
static void test_fault_gates_on_enabled_not_on_dedication(void) {
	given_operable();
	plug_lock.bricklet_dedication_verified = false;
	plug_lock_tick();

	CHECK(!plug_lock_dedication_is_currently_verified()); // hardware gone
	CHECK(plug_lock_has_fault());                        // ... and that is a fault
}

// plug_lock_init() runs after evse_init() has already read enabled out of the
// config page, so its memset must not wipe it. Mirrors button_init().
static void test_init_preserves_persisted_enabled(void) {
	memset(&hardware_version, 0, sizeof(hardware_version));
	hardware_version.is_v4 = true;
	stub_now_ms            = 500;

	memset(&plug_lock, 0, sizeof(plug_lock));
	plug_lock.enabled                      = true; // as loaded from eeprom
	plug_lock.bricklet_dedication_verified = true; // stale leftovers that must be cleared
	plug_lock.report_time                  = 400;
	plug_lock.lock_wanted      = true;

	plug_lock_init();

	CHECK(plug_lock.enabled);
	CHECK(!plug_lock.bricklet_dedication_verified);
	CHECK(plug_lock.report_time == 0);
	CHECK(!plug_lock.lock_wanted);
	CHECK(plug_lock.state == PLUG_LOCK_STATE_DISABLED);

	// A disabled feature must survive init as disabled.
	memset(&plug_lock, 0, sizeof(plug_lock));
	plug_lock_init();
	CHECK(!plug_lock.enabled);
}

// lock_wanted = car_detected || !contactor_confirmed_open. The trigger is the car
// being detected rather than the contactor being requested, so that "plugged in but
// not charging" - scheduled charging, for hours - holds the cable.
static void test_lock_wanted_follows_the_car(void) {
	given_operable();
	plug_lock_tick();

	CHECK(!plug_lock.lock_wanted);
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_IDLE);

	given_car_connected();
	plug_lock_tick();

	CHECK(plug_lock.lock_wanted);
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_WAITING);

	// The ESP32 reports the plug locked. Still wanted, now confirmed.
	plug_lock.lock_closed = true;
	plug_lock_tick();

	CHECK(plug_lock.lock_wanted);
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_LOCKED);

	// Unplugged at the car end: the same signal that locked it releases it.
	given_no_car();
	plug_lock_tick();

	CHECK(!plug_lock.lock_wanted);
}

// Commanded open is not open. A welded contactor keeps its aux contact engaged, and
// that is exactly the case where releasing the plug is dangerous: the socket may be
// live while the plug becomes removable.
static void test_lock_held_until_contactor_confirmed_open(void) {
	given_operable();
	given_contactor_closed();
	plug_lock_tick();

	CHECK(plug_lock.lock_wanted); // no car, but the contactor is still engaged

	given_contactor_open();
	plug_lock_tick();

	CHECK(!plug_lock.lock_wanted);
}

// While contactor_check is still counting down its settling window after a switching
// transient, neither direction is confirmed. That must leave the lock engaged and
// must not raise the mid-session fault.
static void test_contactor_settling_window_is_conservative(void) {
	given_operable();
	contactor_check.invalid_counter = 5;
	plug_lock.lock_closed           = false;
	plug_lock_tick();

	CHECK(plug_lock.lock_wanted);  // not confirmed open -> keep it locked
	CHECK(!plug_lock_has_fault()); // not confirmed closed -> no mid-session fault
}

// The recorded deadlock: a plug lock fault latches iec61851 in state EF for as long
// as the fault lasts, and the fault only clears when the car is disconnected. Keying
// the lock off iec61851.state would therefore hold the cable hostage forever. The
// physical CP/PE reading has to win.
static void test_latched_ef_state_does_not_trap_the_cable(void) {
	given_operable();
	given_car_connected();
	plug_lock_tick();
	CHECK(plug_lock.lock_wanted);

	// Fault: the plug never locked, the state machine is pinned at EF. The ESP32 keeps
	// heartbeating throughout, so this is the backstop firing and not staleness.
	stub_now_ms          += PLUG_LOCK_CLOSE_TIMEOUT_MS;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();
	CHECK(plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_FAULT_TIMEOUT);

	iec61851.state = IEC61851_STATE_EF;
	plug_lock_tick();
	CHECK(plug_lock.lock_wanted); // car still attached

	// User pulls the cable at the car end. iec61851.state is still EF, but the
	// resistance says the car is gone, so the plug must be released.
	adc_result.cp_pe_resistance = 100000;
	plug_lock_tick();

	CHECK(!plug_lock.lock_wanted);
	CHECK(!plug_lock_has_fault()); // and the latched timeout clears with it
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_IDLE);
}

// No trustworthy CP reading means no release. Each of these three says "the number in
// adc_result does not currently describe the socket".
static void test_untrustworthy_cp_reading_keeps_the_lock(void) {
	given_operable();

	stub_cp_connected = false;
	plug_lock_tick();
	CHECK(plug_lock.lock_wanted);

	given_no_car();
	adc_result.cp_pe_is_ignored = true;
	plug_lock_tick();
	CHECK(plug_lock.lock_wanted);

	given_no_car();
	iec61851.force_state_f = true;
	plug_lock_tick();
	CHECK(plug_lock.lock_wanted);
}

// The backstop exists for an ESP32 that is alive but not doing its job: still
// heartbeating, so staleness never fires, but never reporting either success or a
// lock fault. Without it the EVSE would veto silently forever.
static void test_backstop_timeout_fires_and_latches(void) {
	given_operable();
	given_car_connected();
	plug_lock_tick();

	CHECK(!plug_lock.close_timed_out);

	stub_now_ms += PLUG_LOCK_CLOSE_TIMEOUT_MS - 1;
	plug_lock.report_time = stub_now_ms; // keep heartbeating
	plug_lock_tick();

	CHECK(!plug_lock.close_timed_out);
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_WAITING);

	stub_now_ms += 1;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(plug_lock.close_timed_out);
	CHECK(plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_FAULT_TIMEOUT);

	// Latched: a late report of success must not clear it on its own. The EVSE never
	// retries by itself; the user has to disconnect.
	plug_lock.lock_closed = true;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(plug_lock.close_timed_out);
	CHECK(plug_lock_has_fault());

	// Disconnecting clears it, and the next connection starts from scratch.
	given_no_car();
	plug_lock_tick();
	CHECK(!plug_lock.close_timed_out);
	CHECK(!plug_lock_has_fault());
}

// The backstop must not fire while the plug is not even supposed to be locked.
static void test_backstop_does_not_fire_without_lock_wanted(void) {
	given_operable();
	stub_now_ms += PLUG_LOCK_CLOSE_TIMEOUT_MS * 10;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock.lock_wanted);
	CHECK(!plug_lock.close_timed_out);
	CHECK(!plug_lock_has_fault());
}

// A cold boot with the feature enabled. adc_result is still zeroed, so cp_pe_resistance
// reads 0 and plug_lock_car_detected() concludes a car is attached, which puts lock_wanted
// true on the very first tick - while the ESP32 needs the better part of twenty seconds to
// boot and send its first report. Counting the backstop through that gap latched a
// spurious FAULT_TIMEOUT on every power-up, held until lock_wanted next went false. Nobody
// has reported anything yet, so the window must not run at all.
static void test_backstop_does_not_run_before_the_first_report(void) {
	memset(&hardware_version, 0, sizeof(hardware_version));
	hardware_version.is_v4 = true;

	memset(&contactor_check, 0, sizeof(contactor_check));
	memset(&iec61851, 0, sizeof(iec61851));
	memset(&adc_result, 0, sizeof(adc_result)); // as it is at boot: cp_pe_resistance == 0
	memset(stub_gpio_input, 0, sizeof(stub_gpio_input));
	given_contactor_open();
	stub_cp_connected = true;

	stub_now_ms = 12;

	memset(&plug_lock, 0, sizeof(plug_lock));
	plug_lock.enabled = true; // restored from eeprom by evse_init()

	plug_lock_tick();

	CHECK(plug_lock.lock_wanted); // the zeroed ADC reads as a car attached
	CHECK(!plug_lock.close_timed_out);

	// Well past the backstop, still before the ESP32's first report.
	stub_now_ms += PLUG_LOCK_CLOSE_TIMEOUT_MS * 2;
	plug_lock_tick();

	CHECK(!plug_lock.close_timed_out);
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_BRICKLET_DEDICATION_NOT_VERIFIED);

	// The ESP32 arrives. The window starts from here, not from boot.
	plug_lock.bricklet_dedication_verified = true;
	plug_lock.report_time                  = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock.close_timed_out);
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_WAITING);

	// And then it does its job as normal, counted from the first report.
	stub_now_ms          += PLUG_LOCK_CLOSE_TIMEOUT_MS + 1;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(plug_lock.close_timed_out);
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_FAULT_TIMEOUT);
}

// The backstop measures the ESP32's opportunity to act, so the window restarts when the
// ESP32 comes back rather than expiring unobserved while it is away. Without that, an
// ESP32 that reboots with the plug held latches the backstop on its very first report -
// which carries lock_closed = false until its feedback input debounces - and only a
// disconnect at the car end clears it, even though the plug never moved.
static void test_backstop_window_restarts_when_the_esp32_returns(void) {
	given_operable();
	given_car_connected();
	plug_lock.lock_closed = true;
	plug_lock_tick();

	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_LOCKED);
	CHECK(!plug_lock_has_fault());

	// Away for longer than both windows: nothing refreshes report_time.
	stub_now_ms += PLUG_LOCK_CLOSE_TIMEOUT_MS * 2;
	plug_lock_tick();

	CHECK(!plug_lock.close_timed_out);
	// Not a fault: this is the reboot case, the contactor is open and twenty seconds of
	// silence is well inside the absence grace. The gap is not unguarded - the veto holds
	// throughout, which is the part that actually matters.
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_contactor_close_is_blocked());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_BRICKLET_DEDICATION_NOT_VERIFIED);

	// Back, reporting before its feedback input has debounced.
	plug_lock.lock_closed = false;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock.close_timed_out);
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_WAITING);

	// Debounced a moment later, and the session carries on without a disconnect.
	stub_now_ms          += 500;
	plug_lock.lock_closed = true;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(!plug_lock.close_timed_out);
	CHECK(!plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_LOCKED);

	// A full window, not an exemption: an ESP32 that returns and then fails to lock is
	// still caught, counted from the moment it came back.
	plug_lock.lock_closed = false;
	stub_now_ms          += PLUG_LOCK_CLOSE_TIMEOUT_MS + 1;
	plug_lock.report_time = stub_now_ms;
	plug_lock_tick();

	CHECK(plug_lock.close_timed_out);
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_FAULT_TIMEOUT);
}

// lock_fault means "failed to lock" only. It is latched for the same reason the
// timeout is, and cleared the same way.
static void test_reported_lock_fault_blocks_and_clears_on_disconnect(void) {
	given_operable();
	given_car_connected();
	plug_lock.lock_fault = true; // as FID_SET_PLUG_LOCK_HARDWARE_STATE sets it
	plug_lock_tick();

	CHECK(plug_lock_has_fault());
	CHECK(plug_lock_get_state() == PLUG_LOCK_STATE_FAULT_LOCK);

	given_no_car();
	plug_lock_tick();

	CHECK(!plug_lock.lock_fault);
	CHECK(!plug_lock_has_fault());
}

// The gap the veto cannot close. The veto only ever guards a *request* to close the
// contactor, so it cannot drop one that is already closed. Feedback going away while
// the socket is live means the plug came unlocked mid-session.
static void test_mid_session_lock_loss_is_a_fault(void) {
	given_operable();
	given_car_connected();
	plug_lock.lock_closed = true;
	plug_lock_tick();
	CHECK(!plug_lock_has_fault());

	given_contactor_closed(); // charging
	plug_lock_tick();
	CHECK(!plug_lock_has_fault());

	plug_lock.lock_closed = false; // the plug came unlocked while live
	plug_lock_tick();

	CHECK(plug_lock_has_fault());
}

// The veto: no energizing until the plug is confirmed locked, and it must gate on
// `enabled` rather than on a verified dedication, for the same reason the fault does.
// Losing a bricklet mid-session must leave the veto engaged, not remove it.
static void test_veto_blocks_until_locked(void) {
	given_operable();
	given_car_connected();
	plug_lock_tick();

	CHECK(plug_lock_contactor_close_is_blocked());

	plug_lock.lock_closed = true;
	plug_lock_tick();
	CHECK(!plug_lock_contactor_close_is_blocked());

	// Hardware gone: the dedication is no longer verified, but the veto has to stay.
	plug_lock.lock_closed                  = false;
	plug_lock.bricklet_dedication_verified = false;
	plug_lock_tick();

	CHECK(!plug_lock_dedication_is_currently_verified());
	CHECK(plug_lock_contactor_close_is_blocked());

	// Disabled or not v4, it never blocks anything.
	plug_lock.enabled = false;
	plug_lock_tick();
	CHECK(!plug_lock_contactor_close_is_blocked());

	given_operable();
	hardware_version.is_v4 = false;
	plug_lock_tick();
	CHECK(!plug_lock_contactor_close_is_blocked());
}

// The gate is the harness, not the session. Gating on "a car is attached right now"
// would only protect the session in progress: disabling at state A is accepted, the
// flag persists, and the socket then energizes with a removable plug. So the guard
// holds for as long as a plug lock is proven fitted, and the only way out is to
// disconnect the harness loop with the enclosure open.
static void test_disable_is_blocked_guards_the_disable_path(void) {
	given_operable();
	plug_lock_tick();
	CHECK(plug_lock_disable_is_blocked()); // hardware proven, nothing else matters

	// Deliberately insensitive to the session. All four of these used to change the
	// answer and must not any more.
	given_car_connected();
	plug_lock_tick();
	CHECK(plug_lock_disable_is_blocked());

	given_no_car();
	given_contactor_closed();
	plug_lock_tick();
	CHECK(plug_lock_disable_is_blocked());

	given_contactor_open();
	plug_lock_tick();
	CHECK(plug_lock_disable_is_blocked());

	// THE REGRESSION THIS GUARD EXISTS FOR. The lock supply dies: the harness loop
	// de-asserts, but both bricklets keep their own supply from the bricklet port and
	// are still found. Charging must stop AND the disable must stay refused - accepting
	// it here would let a blown fuse do what is meant to need someone at the enclosure.
	given_operable();
	plug_lock.bricklet_dedication_verified = false;
	plug_lock.bricklets_not_found          = false;
	plug_lock.report_time                  = stub_now_ms;
	plug_lock_tick();
	CHECK(!plug_lock_dedication_is_currently_verified());
	CHECK(plug_lock_has_fault());
	CHECK(plug_lock_disable_is_blocked());

	// The escape hatch: both bricklets unplugged, freshly reported. Only now is the
	// disable accepted, and by that point charging is already blocked, so it can only
	// clear a standing fault rather than switch off a lock that still works.
	given_operable();
	plug_lock.bricklet_dedication_verified = false;
	plug_lock.bricklets_not_found          = true;
	plug_lock.report_time                  = stub_now_ms;
	plug_lock_tick();
	CHECK(plug_lock_bricklets_are_currently_not_found());
	CHECK(!plug_lock_disable_is_blocked());
	CHECK(plug_lock_has_fault());

	// One bricklet left is a partly disassembled lock, not a removed one. The ESP32
	// sends false for both, so this is the same shape as the supply failure above.
	given_operable();
	plug_lock.bricklet_dedication_verified = false;
	plug_lock.bricklets_not_found          = false;
	plug_lock.report_time                  = stub_now_ms;
	plug_lock_tick();
	CHECK(plug_lock_disable_is_blocked());

	// Silence is not permission. A stale "both gone" is no evidence at all, so the
	// gate closes again - which also covers a dead ESP32 and a broken SPI link.
	given_operable();
	plug_lock.bricklets_not_found = true;
	stub_now_ms += PLUG_LOCK_ABSENCE_GRACE_MS + 1;
	plug_lock_tick();
	CHECK(!plug_lock_bricklets_are_currently_not_found());
	CHECK(plug_lock_disable_is_blocked());
	CHECK(plug_lock_has_fault());

	// It says nothing once the feature is off: there is no lock to keep.
	given_operable();
	plug_lock.enabled = false;
	plug_lock_tick();
	CHECK(!plug_lock_disable_is_blocked());

	// Nor on v2/v3, where the feature cannot run at all.
	given_operable();
	hardware_version.is_v4 = false;
	plug_lock_tick();
	CHECK(!plug_lock_disable_is_blocked());
}

// The mirror of the disable guard. Enabling the feature while the contactor is closed
// would fault the running session on the very next tick, because the lock cannot be
// confirmed closed at the instant it is switched on. FID 78 refuses instead.
static void test_enable_is_blocked_while_contactor_closed(void) {
	given_operable();
	plug_lock.enabled = false; // the state the guard is consulted in
	plug_lock_tick();

	CHECK(!plug_lock_enable_is_blocked()); // contactor confirmed open -> fine

	given_contactor_closed();
	CHECK(plug_lock_enable_is_blocked());

	// Still blocked while contactor_check has not settled after a switching transient.
	given_contactor_open();
	contactor_check.invalid_counter = 5;
	CHECK(plug_lock_enable_is_blocked());

	// A car connected with the contactor open is deliberately still allowed: the lock
	// engages within its budget and the fault term cannot fire while the contactor is
	// open.
	given_contactor_open();
	given_car_connected();
	CHECK(!plug_lock_enable_is_blocked());

	// Nothing to guard on v2/v3, where the feature cannot run at all.
	given_contactor_closed();
	hardware_version.is_v4 = false;
	hardware_version.is_v3 = true;
	CHECK(!plug_lock_enable_is_blocked());
}

// The guard has to answer while the feature is still off - that is the whole point of
// it - so it must not be gated on plug_lock.enabled the way disable_is_blocked() is.
static void test_enable_is_blocked_not_gated_on_enabled(void) {
	given_operable();
	given_contactor_closed();

	plug_lock.enabled = false;
	CHECK(plug_lock_enable_is_blocked());

	plug_lock.enabled = true;
	CHECK(plug_lock_enable_is_blocked());
}

// Pins the reason the guard exists: the condition it blocks is exactly the condition
// that makes plug_lock_has_fault() fire the instant the feature is switched on. If
// these two ever stop agreeing, enabling mid-session silently starts killing charges
// again.
static void test_enable_is_blocked_matches_the_fault_it_prevents(void) {
	given_operable();
	given_contactor_closed();

	// The moment of enabling: nothing has asked the ESP32 to lock anything yet.
	plug_lock.enabled     = true;
	plug_lock.lock_closed = false;
	plug_lock_tick();

	CHECK(plug_lock_has_fault());

	// ... and the guard would have refused that transition.
	plug_lock.enabled = false;
	CHECK(plug_lock_enable_is_blocked());
}

typedef struct {
	const char *name;
	void (*run)(void);
} Test;

static const Test tests[] = {
	{ "idle when enabled and assertion fresh",     test_idle_when_enabled_and_fresh            },
	{ "disabled is inert",                         test_disabled_is_inert                      },
	{ "not v4 is inert",                           test_not_v4_is_inert                        },
	{ "boot blocks without faulting",              test_boot_before_first_assertion_blocks_without_faulting},
	{ "absence window runs from the report",       test_absence_window_runs_from_the_last_report},
	{ "absence grace never covers a live socket",  test_absence_grace_does_not_cover_a_closed_contactor},
	{ "startup claim covers an early report",      test_startup_claim_covers_an_early_unverified_report},
	{ "settled and unverified faults at once",     test_settled_and_unverified_faults_at_once  },
	{ "startup claim is bounded",                  test_startup_claim_is_bounded               },
	{ "veto refuses a stale lock_closed",          test_veto_refuses_a_stale_lock_closed       },
	{ "shutdown request is reported",              test_shutdown_request_is_reported           },
	{ "shutdown claim is bounded when alive",      test_shutdown_claim_is_bounded_while_the_esp32_is_alive},
	{ "shutdown request inert when disabled",      test_shutdown_request_is_inert_when_disabled},
	{ "shutdown request never hides a fault",      test_shutdown_request_never_suppresses_a_fault},
	{ "assertion goes stale at the deadline",      test_assertion_goes_stale                   },
	{ "fresh assertion recovers from stale",       test_fresh_assertion_recovers               },
	{ "survives ms counter wraparound",            test_survives_millisecond_counter_wraparound},
	{ "fault gates on enabled, not dedication",    test_fault_gates_on_enabled_not_on_dedication},
	{ "init preserves persisted enabled",          test_init_preserves_persisted_enabled       },
	{ "lock_wanted follows the car",               test_lock_wanted_follows_the_car            },
	{ "lock held until contactor confirmed open",  test_lock_held_until_contactor_confirmed_open},
	{ "contactor settling window is conservative", test_contactor_settling_window_is_conservative},
	{ "latched EF does not trap the cable",        test_latched_ef_state_does_not_trap_the_cable},
	{ "untrustworthy CP reading keeps the lock",   test_untrustworthy_cp_reading_keeps_the_lock},
	{ "backstop timeout fires and latches",        test_backstop_timeout_fires_and_latches     },
	{ "backstop needs lock_wanted",                test_backstop_does_not_fire_without_lock_wanted},
	{ "backstop idle before the first report",     test_backstop_does_not_run_before_the_first_report},
	{ "backstop restarts when ESP32 returns",      test_backstop_window_restarts_when_the_esp32_returns},
	{ "reported lock fault blocks and clears",     test_reported_lock_fault_blocks_and_clears_on_disconnect},
	{ "mid-session lock loss is a fault",          test_mid_session_lock_loss_is_a_fault       },
	{ "veto blocks until locked",                  test_veto_blocks_until_locked               },
	{ "disable_is_blocked guards disable path",    test_disable_is_blocked_guards_the_disable_path},
	{ "enable blocked while contactor closed",     test_enable_is_blocked_while_contactor_closed},
	{ "enable block not gated on enabled",         test_enable_is_blocked_not_gated_on_enabled },
	{ "enable block matches the fault it stops",   test_enable_is_blocked_matches_the_fault_it_prevents},
};

int main(void) {
	const int count = (int)(sizeof(tests) / sizeof(tests[0]));
	int failed_tests = 0;

	printf("plug_lock_test: %d tests\n\n", count);

	for(int i = 0; i < count; i++) {
		const int checks_before   = checks;
		const int failures_before = failures;

		printf("  %-44s", tests[i].name);
		name_line_open = true;

		tests[i].run();

		const int ran    = checks - checks_before;
		const int failed = failures - failures_before;

		if(failed == 0) {
			printf(" ok (%d checks)\n", ran);
		} else {
			failed_tests++;
			printf("      -> FAIL (%d of %d checks)\n", failed, ran);
		}

		name_line_open = false;
	}

	if(failures > 0) {
		printf("\n%d tests, %d checks, %d failed in %d test(s)\n",
		       count, checks, failures, failed_tests);
		return 1;
	}

	printf("\n%d tests, %d checks, all ok\n", count, checks);

	return 0;
}
