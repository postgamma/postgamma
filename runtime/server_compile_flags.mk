# Checked role-state access adds code to otherwise small PostgreSQL helpers.
# With GCC's -O2 budget of 15, this can turn an inlined loop body into a call
# on every iteration. Allow a small amount of extra code without changing the
# optimization level, unit-growth limit, runtime checks or individual functions.
# Query the active compiler: do not lower a larger native budget or pass GCC
# parameters to Clang. Explicit caller tuning always takes precedence.
ifeq ($(POSTGAMMA_DIRECT_ROLE_STATE),1)
ifeq ($(lastword $(filter -O%,$(CFLAGS))),-O2)
ifeq ($(findstring max-inline-insns-auto,$(CFLAGS))$(findstring -finline-limit,$(CFLAGS)),)
postgamma_state_inline_cflags := $(shell $(CC) $(CFLAGS) -Q --help=params \
	-x c -S -o /dev/null /dev/null 2>/dev/null | \
	awk '$$1 == "--param=max-inline-insns-auto=" && $$2 ~ /^[0-9]+$$/ && $$2 < 20 \
	{ print "--param=max-inline-insns-auto=20" }')
override CFLAGS += $(postgamma_state_inline_cflags)
endif
endif
endif
