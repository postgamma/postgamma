# Public product entry points. Keep preparation ordered even under make -j;
# nested compilers retain the caller's JOBS and toolchain/output overrides.
.PHONY: server-build c-build python-build wasm-build
.PHONY: server-test c-test python-test full-test full-test-prepare
.PHONY: server-doctor c-doctor python-doctor wasm-doctor docs-doctor full-test-doctor doctor

# Doctors only inspect installed prerequisites and run bounded probes. Source
# initialization and downloads belong to the build/setup/prepare entry points.
server-doctor: doctor-threaded
c-doctor: doctor-sdk
python-doctor: doctor-python
docs-doctor: doctor-docs

wasm-doctor:
	@$(MAKE) doctor-ast
	@$(MAKE) wasm-toolchain-doctor
	@$(MAKE) wasm-generator-tools-check

# Continue independent diagnostics after failures, but never report success if
# any scope failed. Native release packaging uses doctor-release instead.
doctor:
	@doctor_status=0; for doctor_target in doctor-native wasm-doctor wasm-browser-doctor; do \
		$(MAKE) "$$doctor_target" || doctor_status=1; \
	done; exit $$doctor_status

full-test-doctor:
	@doctor_status=0; for doctor_target in doctor-full-test python-doctor docs-doctor wasm-doctor wasm-browser-doctor; do \
		$(MAKE) "$$doctor_target" || doctor_status=1; \
	done; exit $$doctor_status

server-build:
	@$(MAKE) source-init
	@$(MAKE) threaded-postgresql-build

c-build:
	@$(MAKE) source-init
	@$(MAKE) static-sdk-package

python-build:
	@$(MAKE) source-init
	@$(MAKE) python-wheel

# Internal probe targets keep their debug default. The public SDK build uses
# release unless WASM_MODE was explicitly supplied on the command line or in
# the environment; generation and packaging must always use the same mode.
POSTGAMMA_WASM_BUILD_MODE = $(if $(filter command line environment override,$(origin WASM_MODE)),$(WASM_MODE),release)
wasm-build:
	@$(MAKE) source-init
	@$(MAKE) wasm-toolchain-setup
	@$(MAKE) wasm-doctor
	@$(MAKE) wasm-kernel-generate WASM_MODE="$(POSTGAMMA_WASM_BUILD_MODE)"
	@$(MAKE) wasm-package WASM_MODE="$(POSTGAMMA_WASM_BUILD_MODE)"

server-test:
	@$(MAKE) source-init
	@$(MAKE) threaded-postgresql-check

c-test:
	@$(MAKE) source-init
	@$(MAKE) embedded-runtime-check

python-test:
	@$(MAKE) source-init
	@$(MAKE) python-package-check

# Check browser/toolchain availability before the long native gate. Setup
# reuses matching downloads and never installs system packages with sudo.
full-test-prepare:
	@$(MAKE) source-init
	@$(MAKE) wasm-setup
	@$(MAKE) wasm-browser-setup
	@$(MAKE) full-test-doctor WASM_MODE=release

# complete-source-check remains the native-only gate used by native CI and
# upstream canaries. Run both product gates serially and propagate any failure.
# The multi-hour alpha endurance gate stays explicit: make wasm-alpha-test.
full-test: full-test-prepare
	@$(MAKE) complete-source-check
	@$(MAKE) wasm-test
