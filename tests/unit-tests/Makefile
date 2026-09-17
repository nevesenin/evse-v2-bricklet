# Host unit tests for the parts of the firmware that have no hardware dependency.
#
# These compile for the host, not for the XMC, so they need no ARM toolchain and
# none of the bricklib2/xmclib symlinks that `make` in software/ requires.
# `make` runs them; `make all` only builds them.
#
# Quoted includes resolve relative to the including file before any -I directory,
# so plug_lock.c picks up the *real* plug_lock.h, hardware_version.h, adc.h, evse.h
# and iec61851.h out of software/src. Only the hardware layer is stubbed, and for
# anything under bricklib2/ that needs the forced include - see stubs/force_stubs.h.

CC ?= gcc

SRC := ../../software/src

# Mirrors the warning set in software/CMakeLists.txt, so a test build fails on
# anything the firmware build would fail on.
CFLAGS := -std=gnu99 -Os -Istubs -I$(SRC) -include stubs/force_stubs.h \
	-Wall -Wextra -Wdouble-promotion -Wfloat-conversion -Wshadow \
	-Wstrict-prototypes -Wno-unused-parameter -Wcast-align -Wnull-dereference \
	-Wundef -Wsign-conversion -Werror

# The firmware build also uses these, but they are GCC-only: clang rejects them
# outright via -Wunknown-warning-option, which -Werror then turns into an error.
# Probe each one instead of assuming the compiler, so the tests build with either.
cc-option = $(shell $(CC) $(1) -Werror -E -x c - </dev/null >/dev/null 2>&1 && echo $(1))

CFLAGS += $(foreach flag,\
	-Wduplicated-cond -Wduplicated-branches -Wjump-misses-init -Wshift-overflow=2,\
	$(call cc-option,$(flag)))

TESTS := plug_lock_test

# Running the tests is the useful default; plain `make` otherwise just reports
# that the already-built binary is up to date.
.DEFAULT_GOAL := check

all: $(TESTS)

plug_lock_test: plug_lock_test.c $(SRC)/plug_lock.c $(SRC)/plug_lock.h
	$(CC) $(CFLAGS) -o $@ plug_lock_test.c $(SRC)/plug_lock.c

check: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(TESTS)

.PHONY: all check clean
