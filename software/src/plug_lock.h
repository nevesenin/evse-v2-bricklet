/* evse-v2-bricklet
 * Copyright (C) 2026 empunkt <empunkt@mailbox.org>
 *
 * plug_lock.h: Type 2 plug lock driven by bricklets on the ESP32
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

#ifndef PLUG_LOCK_H
#define PLUG_LOCK_H

#include <stdint.h>
#include <stdbool.h>

// The plug lock is actuated by two bricklets on the ESP32 (Industrial Quad Relay 2.1
// and Industrial Digital In 4 2.0), not by any IO on this board. The EVSE cannot reach
// bricklets (it is an SPI slave), so it never senses the lock itself. Instead the ESP32
// asserts the hardware state and this module holds the energizing authority:
//
//   - it decides *when* the plug must be locked (lock_wanted)
//   - it refuses to energize without a fresh assertion that the lock is closed
//
// The actuation state machine lives on the ESP32, next to its own IO.
//
// We never try to detect the ESP32's absence. Energizing requires fresh positive
// evidence, and the absence of evidence is the block, so every failure mode
// (crash, hang, reboot, SPI fault, bricklet unplugged, brick powered down) collapses
// into the same observable: the assertion stops arriving.
//
// Note "the block". Absence blocks energizing; it is not by itself a claim that
// anything is wrong. The two are separate mechanisms here -
// plug_lock_contactor_close_is_blocked() and plug_lock_has_fault() - and only the first
// follows from absence alone. See PLUG_LOCK_ABSENCE_GRACE_MS.
#define PLUG_LOCK_REPORT_STALE_MS 5000

// How long silence may last before it is a failure rather than a start-up, a restart or
// a blip on the SPI link. None of those is anomalous, and all three look identical from
// here - the only thing that tells them apart is duration. Energizing stays blocked
// throughout, because that is plug_lock_contactor_close_is_blocked()'s job and it refuses
// on a stale assertion; what this bounds is the *fault*, never the veto.
//
// Measured: a cold boot reaches its first dedication-verified report 4.39 s and 4.64 s
// after power-on, and a restart is back in about the same (2026-09-13). A boot that also
// has to flash a Bricklet takes considerably longer. 60 s is far outside all of those on
// purpose - nothing can be energized while it runs, so the only cost of a generous window
// is a later error message on a charger that is genuinely broken, against the cost of too
// short a one, which is the spurious fault on every single boot that this removes.
//
// One window covers both the case where nothing has ever been reported and the case where
// reports stopped, because it is the same question. They differ only in what there is to
// measure from: see plug_lock_absence_is_prolonged().
//
// This is also what makes an explicit shutdown honest rather than merely quiet: with it,
// the charger can interrupt charging deliberately and say nothing is wrong, because
// nothing is. Without it the staleness fault would fire five seconds later and contradict
// that.
#define PLUG_LOCK_ABSENCE_GRACE_MS 60000

// How long a claimed transition is believed. The ESP32 makes exactly two claims that
// change what this module does - "I have not settled yet" and "I am restarting" - and
// both are bounded here rather than trusted, because both are assertions that relax
// behaviour: the first delays a fault, the second holds a session off.
//
// The bound is not about mistrust so much as about contradiction. A claim describes a
// transition, and a transition that is still being claimed thirty seconds later is not
// happening. Measured start-ups settle in about 4.5 s and restarts are back in about the
// same, so this is an outer bound with a wide margin, not a budget.
//
// Deliberately longer than the ESP32's own PLUG_LOCK_STARTUP_SETTLE_TIMEOUT (20 s, in
// plug_lock.cpp), so the side making the claim always gives up before the side believing
// it does. Keep that ordering if either number changes: the reverse would make the EVSE
// fault on an ESP32 that was about to stop claiming anyway.
#define PLUG_LOCK_CLAIM_TIMEOUT_MS 30000

// Backstop from the lock_wanted false->true edge. This is not the same job as the
// ESP32's own ~5 s per-attempt timeout: that one is the diagnostic (it commanded the
// relay, so it knows the cause and logs it), this one is the guarantee. An ESP32 that
// is alive but not doing its job - still heartbeating, so staleness never fires, but
// stuck in its state machine and reporting neither success nor lock_fault - would
// otherwise leave the EVSE vetoing silently forever.
//
// Must stay comfortably longer than the ESP32's timeout plus its retries (measured
// budget there is ~6.5 s), or this fires first and the specific error message is lost.
//
// The window only runs while the report is fresh, and restarts when freshness is
// regained. What it measures is the ESP32's opportunity to act, not wall time: one that
// reboots while the plug is held had no opportunity at all while it was away, and its
// first report after booting necessarily carries lock_closed = false because its
// feedback input has not debounced yet. Counting through the gap would latch the
// backstop on the very report that says the ESP32 is back. The case this exists for -
// alive and still heartbeating - never breaks freshness, so it is unaffected.
#define PLUG_LOCK_CLOSE_TIMEOUT_MS 10000

// One comment per line rather than trailing ones: the constant names are too long for a
// trailing column to stay readable.
typedef enum {
	// Not activated by the user, or not hardware version 4.
	PLUG_LOCK_STATE_DISABLED                         = 0,
	// Activated, but no fresh assertion that the bricklets are dedicated to this lock.
	// That is NOT the same as "the bricklets are gone": a dead lock PSU de-asserts the
	// harness loop with both of them still fitted, and so does a report going stale.
	PLUG_LOCK_STATE_BRICKLET_DEDICATION_NOT_VERIFIED = 1,
	// Activated, dedication verified, the plug does not need to be locked.
	PLUG_LOCK_STATE_IDLE                             = 2,
	// The plug must be locked but is not confirmed locked yet.
	PLUG_LOCK_STATE_WAITING                          = 3,
	// The plug must be locked and is confirmed locked.
	PLUG_LOCK_STATE_LOCKED                           = 4,
	// The plug did not lock within PLUG_LOCK_CLOSE_TIMEOUT_MS.
	PLUG_LOCK_STATE_FAULT_TIMEOUT                    = 5,
	// The ESP32 reported that it failed to lock the plug.
	PLUG_LOCK_STATE_FAULT_LOCK                       = 6,
} PlugLockState;

typedef struct {
	// Persisted in the EVSE config page. User intent, set through FID_SET_PLUG_LOCK_CONFIGURATION.
	bool enabled;

	// Volatile, asserted by the ESP32 through FID_SET_PLUG_LOCK_HARDWARE_STATE. Never persisted.
	//
	// bricklet_dedication_verified does NOT mean "two bricklets answered". It means the
	// ESP32 has verified the harness loop: a spare relay channel wired in series through
	// a spare input channel and back to the lock PSU, toggled and observed to follow.
	// Discovery by device identifier only proves that *some* Quad Relay and *some*
	// Digital In 4 are plugged in, which any user with unrelated bricklets would satisfy.
	// The loop proves these two specific devices are dedicated to this lock - wired to
	// each other and to the lock supply - and it is a wire, so nothing reachable over the
	// network can assert it.
	//
	// That is what makes it the arming signal for *enabling* the feature
	// (communication.c). It is deliberately NOT what guards the reverse direction: the
	// loop is also de-asserted when the lock supply fails while both bricklets are still
	// fitted, so "not verified" cannot be read as "the lock was removed". See
	// bricklets_not_found below.
	bool bricklet_dedication_verified;

	// Neither bricklet could be found. The separate question, and the only signal here
	// that answers it - discovery runs on the bricklet port, which keeps its supply when
	// the lock supply dies, so this stays false through exactly the failure that clears
	// bricklet_dedication_verified.
	//
	// Both, not either: one remaining bricklet is a partly disassembled lock rather than
	// a removed one, and one loose 7-pin cable must not open the disable path.
	//
	// Polarity is chosen so the restrictive answer is what a zero byte gives.
	bool bricklets_not_found;

	bool lock_closed; // the debounced feedback input, not the ESP32's conclusion
	bool lock_fault;  // an attempt to *lock* failed; a failed unlock is never reported here

	// The ESP32 is about to restart on purpose. Asserted in its last report before it
	// goes and cleared by the first report of the instance that comes back, so the last
	// value asserted covers the whole absence without any latching here.
	//
	// It exists to make one thing true: whenever the ESP32 is away by its own choice, the
	// contactor is open. That turns a contactor found closed with no assertion into an
	// unambiguous anomaly instead of the normal outcome of every reboot.
	//
	// Note what it can and cannot do. It selects IEC 61851 state B over state C, which
	// releases the contactor while leaving the plug connected and the PWM running - the
	// ordinary way this charger declines to start or continue a session. It is
	// deliberately NOT consulted by plug_lock_has_fault(): a message from the ESP32 may
	// make this charger more restrictive, never less, so nothing that fails to arrive -
	// a crash, a panic, a power loss, /force_reboot - can leave the charger worse off
	// than not having sent it. Silence still falls through to the ordinary staleness path.
	bool shutdown_requested;
	uint32_t shutdown_claim_time;

	// The ESP32 has not yet had its chance to reach a verdict on the harness loop, so a
	// bricklet_dedication_verified of false means "not yet" and not "checked and broken".
	// Those two are otherwise identical on the wire, and treating the first as the second
	// reported a failure during the first seconds of every start-up.
	//
	// It is a claim, so it is bounded here as well as asserted there: startup_claim_time
	// stamps the edge and plug_lock_startup_claim_holds() stops believing it after
	// PLUG_LOCK_CLAIM_TIMEOUT_MS. It can delay a fault, never prevent one.
	//
	// Polarity, as everywhere else here: a zero byte gives the strict answer, "settled".
	bool esp32_still_starting_up;
	uint32_t startup_claim_time;

	uint32_t report_time;

	// Owned by this module and published upward for the ESP32 to act on. The EVSE
	// decides *when* the plug must be locked, the ESP32 decides *how*.
	bool lock_wanted;
	uint32_t lock_wanted_time; // time of the last false->true edge, for the backstop
	// The backstop fired: the plug was not confirmed locked within
	// PLUG_LOCK_CLOSE_TIMEOUT_MS of lock_wanted going true. Latched, and cleared only
	// when lock_wanted goes false again.
	bool close_timed_out;
	// Whether the previous tick saw a fresh report. The backstop window is restarted on
	// the false->true edge rather than left to expire while nobody is listening.
	bool report_was_fresh;
	// PLUG_LOCK_ABSENCE_GRACE_MS has elapsed since power-on. Latched, never cleared.
	//
	// Only ever read for the one case that has no timestamp to measure from: nothing has
	// ever been reported, so report_time is still 0 and there is no last-heard-from moment
	// to subtract. Latching rather than re-deriving it from the uptime on every tick is
	// what keeps that correct across the 49.7 day millisecond wraparound, which would
	// otherwise re-open the window on a charger that has been running for weeks.
	bool power_on_grace_expired;

	PlugLockState state;
} PlugLock;

extern PlugLock plug_lock;

void plug_lock_init(void);
void plug_lock_tick(void);

// True if bricklet_dedication_verified is set *and* the report carrying it is still
// fresh - "currently" is the freshness, which the struct field itself does not carry.
// Gate for enabling the feature: the user must not be able to switch it on while the
// lock hardware is not proven to be there and wired to this lock.
bool plug_lock_dedication_is_currently_verified(void);

// True if a fresh report says neither bricklet could be found. "currently" is the
// freshness, the same way it is for the dedication above: the struct field is the raw
// assertion, this adds the staleness window. Deliberately not named "gone" - on a first
// boot with no bricklets ever fitted there is nothing to have gone.
bool plug_lock_bricklets_are_currently_not_found(void);

// Consumed by iec61851_tick(). Fail-closed: enabled without a fresh assertion is a fault.
bool plug_lock_has_fault(void);

// Consumed by evse_set_output(). True while the contactor must not be closed because the
// plug is not *freshly* confirmed locked. Only ever consulted for contactor == true;
// de-energizing must never be blocked.
//
// Both halves matter: a stale assertion is not evidence, and this is the layer that has to
// say so on its own rather than leaning on plug_lock_has_fault() to have dropped the
// contactor first.
bool plug_lock_contactor_close_is_blocked(void);

// Consumed by iec61851_tick(). True while the ESP32 has announced that it is restarting
// on purpose. It selects IEC61851_STATE_B over STATE_C, which is the same thing zero
// allowed current does: the plug stays connected and the PWM keeps running, but the
// contactor is released. Unlike the veto above this ends a session that is already
// running, which is why it has to reach the state decision rather than evse_set_output().
//
// Deliberately NOT a charging slot: the slots are a current-limit table that the ESP32
// reads back, and this is not a limit. Deliberately NOT in the fault chain either - see
// the comment at the call site.
bool plug_lock_shutdown_is_requested(void);

// True while the feature must not be given up. FID_SET_PLUG_LOCK_CONFIGURATION refuses
// to disable the plug lock while this holds.
//
// This is deliberately NOT a question about the current session. Gating it on "a car is
// attached right now" only protects the session in progress and says nothing about the
// next one: disabling at IEC 61851 state A is accepted, the flag persists, and the
// socket then energizes with a removable plug. The decision and the hazard are separated
// in time, and the decision can be made by an unauthenticated request on the LAN.
//
// So the gate is the hardware instead. A fitted plug lock that is "switched off" is a
// state with no valid physical meaning, and every legitimate reason to disable - socket
// replaced by a fixed cable, dead actuator, decommissioning - already has someone at the
// wallbox with the enclosure open. Unplugging both bricklets costs them nothing.
//
// It gates on bricklets_not_found and NOT on bricklet_dedication_verified, which is the
// whole point. Asserting the loop needs a wire, but de-asserting it needs only a
// failure: a blown fuse on the lock supply drops it with both bricklets still fitted.
// Gating on the loop therefore let a supply fault open a path that is meant to require
// someone at the enclosure. Discovery is the only signal that tells the two apart.
//
// Both gates now demand positive evidence, so a report that stops arriving refuses in
// both directions rather than defaulting to permissive.
//
// WHAT THIS IS NOT: a security boundary. This function and
// FID_SET_PLUG_LOCK_HARDWARE_STATE are reachable through the same proxy, so a caller who
// can already talk to this device can assert the hardware state too. What the gate buys
// is failing closed when the ESP32 is absent, and a single enforcement point for a flag
// that lives here because it must survive an ESP32 reflash. Against a network attacker,
// proxy authentication is the control; this is not.
bool plug_lock_disable_is_blocked(void);

// The mirror of the above, for the other direction: true while the feature must not be
// *enabled*, because the contactor is not confirmed open. Switching it on with the
// contactor closed would fault the running session immediately - see the comment on the
// implementation.
bool plug_lock_enable_is_blocked(void);

PlugLockState plug_lock_get_state(void);

#endif
