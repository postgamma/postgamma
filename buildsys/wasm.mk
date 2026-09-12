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
.PHONY: wasm wasm-check wasm-check-full wasm-serve

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

wasm-clean:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" clean $(POSTGAMMA_WASM_ARGS)

# The source directory named wasm must not make an unimplemented build succeed.
wasm wasm-check wasm-check-full wasm-serve:
	@$(PYTHON) "$(ROOT)/buildsys/wasm.py" kernel $(POSTGAMMA_WASM_ARGS)
