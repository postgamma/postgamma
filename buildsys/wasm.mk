# SDK settings are passed only to the Wasm driver, never to native builds.
WASM_TARGET ?= wasm32-emscripten-pthreads
WASM_MODE ?= debug
WASM_PROBE ?= smoke
WASM_SEED ?= 1
WASM_LONGJMP ?= emscripten
WASM_DEPS_DIR ?= $(ROOT)/.deps/wasm
WASM_PROFILE ?= $(ROOT)/manifests/profiles/wasm-emscripten.json
WASM_EMSDK_DIR ?=
WASM_NODE ?=
WASM_BROWSER_EXECUTABLE ?=
WASM_PLATFORM_CHECKS ?= 0

POSTGAMMA_WASM_ARGS = --root "$(ROOT)" --build-dir "$(BUILD_DIR)" \
	--deps-dir "$(WASM_DEPS_DIR)" --profile "$(WASM_PROFILE)" \
	--target "$(WASM_TARGET)" --mode "$(WASM_MODE)" --jobs "$(JOBS)" \
	--longjmp "$(WASM_LONGJMP)" \
	$(if $(WASM_EMSDK_DIR),--sdk-dir "$(WASM_EMSDK_DIR)") \
	$(if $(WASM_NODE),--node "$(WASM_NODE)") \
	$(if $(WASM_BROWSER_EXECUTABLE),--browser-executable "$(WASM_BROWSER_EXECUTABLE)")

.PHONY: wasm-setup doctor-wasm wasm-probe-build wasm-probe-check wasm-clean
.PHONY: wasm-platform-check wasm-wait-check wasm-recovery-check wasm-opfs-check
.PHONY: wasm-browser-setup wasm-browser-check
.PHONY: wasm-resource-check
.PHONY: wasm-target-check
.PHONY: wasm-kernel-configure wasm-kernel-capture wasm-kernel-generate wasm-runtime-check
.PHONY: wasm-kernel-link
.PHONY: wasm wasm-check wasm-check-full wasm-release wasm-serve wasm-test
WASM_KEEP_GOING ?= 0
# This convenience entry point is deliberately serial by default on small hosts.
# Explicit JOBS or WASM_TEST_JOBS remains caller-owned.
WASM_TEST_JOBS ?= $(if $(filter command line environment override,$(origin JOBS)),$(JOBS),1)
wasm-test:
	@$(MAKE) source-init JOBS="$(WASM_TEST_JOBS)"
	@$(MAKE) wasm-setup JOBS="$(WASM_TEST_JOBS)"
	@ulimit -Sn "$$(ulimit -Hn)" && $(MAKE) wasm-release JOBS="$(WASM_TEST_JOBS)" WASM_KEEP_GOING=1

wasm-setup:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" setup $(POSTGAMMA_WASM_ARGS)

doctor-wasm:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" doctor $(POSTGAMMA_WASM_ARGS)

wasm-probe-build:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" probe-build $(POSTGAMMA_WASM_ARGS) --probe "$(WASM_PROBE)" --seed "$(WASM_SEED)"

wasm-probe-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" probe-check $(POSTGAMMA_WASM_ARGS) --probe "$(WASM_PROBE)" --seed "$(WASM_SEED)"

wasm-platform-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" probe-check $(POSTGAMMA_WASM_ARGS) --probe platform

wasm-wait-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" probe-check $(POSTGAMMA_WASM_ARGS) --probe wait --seed "$(WASM_SEED)"

wasm-recovery-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" probe-check $(POSTGAMMA_WASM_ARGS) --probe recovery

wasm-opfs-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" probe-check $(POSTGAMMA_WASM_ARGS) --probe opfs

wasm-browser-setup:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" browser-setup $(POSTGAMMA_WASM_ARGS)

wasm-browser-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" probe-check $(POSTGAMMA_WASM_ARGS) --probe deployment

wasm-resource-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" resource-check $(POSTGAMMA_WASM_ARGS)

wasm-target-check:
	@$(PYTHON) "$(ROOT)/buildsys/check_wasm_target.py" --root "$(ROOT)" --build-dir "$(BUILD_DIR)" \
		--mode "$(WASM_MODE)" --target "$(WASM_TARGET)" --longjmp "$(WASM_LONGJMP)"

wasm-kernel-configure:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_kernel.py" configure $(POSTGAMMA_WASM_ARGS)

wasm-kernel-capture:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_kernel.py" capture $(POSTGAMMA_WASM_ARGS)

wasm-kernel-generate:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_generate.py" $(POSTGAMMA_WASM_ARGS) $(VERIFY_UPSTREAM_PROBE_ARG)

wasm-runtime-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_runtime.py" $(POSTGAMMA_WASM_ARGS)

wasm-kernel-link:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_link.py" $(POSTGAMMA_WASM_ARGS)

wasm-clean:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" clean $(POSTGAMMA_WASM_ARGS)

wasm:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_startup.py" build $(POSTGAMMA_WASM_ARGS)

wasm-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_startup.py" check $(POSTGAMMA_WASM_ARGS)

wasm-serve:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_startup.py" serve $(POSTGAMMA_WASM_ARGS)

wasm-release:
	@$(MAKE) wasm-browser-setup
	@$(MAKE) wasm-check-full WASM_MODE=release

# Keep the extension comparison before storage relinks the base artifacts.
wasm-check-full:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_ci.py" $(POSTGAMMA_WASM_ARGS) --suite weekly $(VERIFY_UPSTREAM_PROBE_ARG) $(if $(filter 1 true,$(WASM_KEEP_GOING)),--keep-going)

.PHONY: wasm-pglite-check
WASM_PGLITE_ENGINES ?= chromium,firefox,webkit
WASM_PGLITE_STORAGE ?= ephemeral,opfs
wasm-pglite-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_pglite.py" $(POSTGAMMA_WASM_ARGS) --engines "$(WASM_PGLITE_ENGINES)" --storage "$(WASM_PGLITE_STORAGE)"

.PHONY: wasm-sql-check
WASM_SQL_SCENARIO ?= all
WASM_EXTENSIONS ?= none
wasm-sql-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_sql.py" check $(POSTGAMMA_WASM_ARGS) --scenario "$(WASM_SQL_SCENARIO)" --extensions "$(WASM_EXTENSIONS)"

.PHONY: wasm-lifecycle-check
WASM_LIFECYCLE_REPETITIONS ?= 20
wasm-lifecycle-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_lifecycle.py" $(POSTGAMMA_WASM_ARGS) --repetitions "$(WASM_LIFECYCLE_REPETITIONS)"

.PHONY: wasm-storage-check
WASM_STORAGE_SCENARIO ?= storage-all
wasm-storage-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_sql.py" check $(POSTGAMMA_WASM_ARGS) --scenario "$(WASM_STORAGE_SCENARIO)" --extensions "$(WASM_EXTENSIONS)"

.PHONY: wasm-archive-check wasm-contrib-check
WASM_FEATURE_ENGINES ?= chromium,firefox,webkit
wasm-archive-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_features.py" $(POSTGAMMA_WASM_ARGS) --scenario archive --engines "$(WASM_FEATURE_ENGINES)"

wasm-contrib-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_features.py" $(POSTGAMMA_WASM_ARGS) --scenario contrib --engines "$(WASM_FEATURE_ENGINES)"

.PHONY: wasm-vector-check
WASM_VECTOR_SCENARIO ?= vector-all
wasm-vector-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_sql.py" check $(POSTGAMMA_WASM_ARGS) --scenario "$(WASM_VECTOR_SCENARIO)"

.PHONY: wasm-vector-serve
wasm-vector-serve:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_sql.py" serve $(POSTGAMMA_WASM_ARGS) --scenario vector-example

.PHONY: wasm-package wasm-package-check
WASM_PACKAGE_EXTENSIONS ?= standard
wasm-package:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_package.py" build $(POSTGAMMA_WASM_ARGS) --extensions "$(WASM_PACKAGE_EXTENSIONS)"

wasm-package-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_package.py" check $(POSTGAMMA_WASM_ARGS) --extensions "$(WASM_PACKAGE_EXTENSIONS)"

.PHONY: wasm-matrix-check
wasm-matrix-check:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_matrix.py" $(POSTGAMMA_WASM_ARGS)

.PHONY: wasm-ci
wasm-ci:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_ci.py" $(POSTGAMMA_WASM_ARGS) --suite basic $(VERIFY_UPSTREAM_PROBE_ARG) $(if $(filter 1 true,$(WASM_PLATFORM_CHECKS)),--platform-checks)

.PHONY: wasm-weekly wasm-upstream-canary
wasm-weekly:
	@$(MAKE) wasm-upstream-canary CANARY_PROFILE=weekly

wasm-upstream-canary:
	@$(PYTHON) "$(ROOT)/buildsys/wasm_upstream.py" $(POSTGAMMA_WASM_ARGS) \
		--ref "$(PG_REF)" --branch "$(PG_BRANCH)" --canary-profile "$(CANARY_PROFILE)"
