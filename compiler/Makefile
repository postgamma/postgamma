SHELL := /bin/bash

ROOT := $(abspath ..)
BUILD_DIR ?= $(ROOT)/build/tools
PYTHON ?= python3
FIND_TOOL := $(PYTHON) "$(ROOT)/buildsys/find_tool.py"
LLVM_CONFIG ?= $(shell $(FIND_TOOL) llvm-config --root "$(ROOT)")
CLANGXX ?= $(shell $(FIND_TOOL) clang++ --root "$(ROOT)")
CLANG_CPP_LIBRARY ?= $(shell $(FIND_TOOL) clang-cpp-library --root "$(ROOT)")
CLANG_RESOURCE_DIR ?= $(shell $(FIND_TOOL) clang-resource-dir --root "$(ROOT)")
LLVM_CONFIG := $(LLVM_CONFIG)
CLANGXX := $(CLANGXX)
CLANG_CPP_LIBRARY := $(CLANG_CPP_LIBRARY)
CLANG_RESOURCE_DIR := $(CLANG_RESOURCE_DIR)

TARGETS := $(BUILD_DIR)/postgamma-cc $(BUILD_DIR)/postgamma-state-inventory \
	$(BUILD_DIR)/postgamma-inheritance-inventory \
	$(BUILD_DIR)/postgamma-api-inventory
OBJECTS := $(BUILD_DIR)/postgamma_cc.o $(BUILD_DIR)/postgamma_state_inventory.o \
	$(BUILD_DIR)/postgamma_inheritance_inventory.o \
	$(BUILD_DIR)/postgamma_api_inventory.o
DEPENDENCIES := $(OBJECTS:.o=.d)

LLVM_CXXFLAGS := $(if $(strip $(LLVM_CONFIG)),$(shell $(LLVM_CONFIG) --cxxflags 2>/dev/null))
LLVM_LDFLAGS := $(if $(strip $(LLVM_CONFIG)),$(shell $(LLVM_CONFIG) --ldflags 2>/dev/null))
# libclang-cpp is a shared library on supported distro toolchains.  Link the
# matching shared LLVM library as well; mixing libclang-cpp with LLVM's static
# archives registers LLVM command-line options twice and aborts at startup.
LLVM_LIBS := $(if $(strip $(LLVM_CONFIG)),$(shell $(LLVM_CONFIG) --link-shared --libs --system-libs 2>/dev/null))

CPPFLAGS += -I$(ROOT)/compiler/include \
	-DPOSTGAMMA_CLANG_RESOURCE_DIR='"$(CLANG_RESOURCE_DIR)"'
CXXFLAGS += -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Wno-unused-parameter
LDLIBS += $(if $(strip $(CLANG_CPP_LIBRARY)),$(CLANG_CPP_LIBRARY),-lclang-cpp) $(LLVM_LIBS)

.PHONY: all clean compiler-toolchain-check

all: $(TARGETS)

$(TARGETS) $(OBJECTS): | compiler-toolchain-check

compiler-toolchain-check:
	@test -n "$(LLVM_CONFIG)" || { \
		echo "compiler: llvm-config discovery failed; run make doctor-ast" >&2; \
		exit 2; \
	}
	@test -n "$(CLANGXX)" || { \
		echo "compiler: clang++ discovery failed; run make doctor-ast" >&2; \
		exit 2; \
	}
	@test -n "$(CLANG_CPP_LIBRARY)" || { \
		echo "compiler: libclang-cpp discovery failed; run make doctor-ast" >&2; \
		exit 2; \
	}
	@test -n "$(CLANG_RESOURCE_DIR)" || { \
		echo "compiler: Clang resource directory discovery failed; run make doctor-ast" >&2; \
		exit 2; \
	}

$(BUILD_DIR)/postgamma-cc: $(BUILD_DIR)/postgamma_cc.o
	$(CLANGXX) $< $(LLVM_LDFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/postgamma-state-inventory: $(BUILD_DIR)/postgamma_state_inventory.o
	$(CLANGXX) $< $(LLVM_LDFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/postgamma-inheritance-inventory: $(BUILD_DIR)/postgamma_inheritance_inventory.o
	$(CLANGXX) $< $(LLVM_LDFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/postgamma-api-inventory: $(BUILD_DIR)/postgamma_api_inventory.o
	$(CLANGXX) $< $(LLVM_LDFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/postgamma_cc.o: src/postgamma_cc.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CLANGXX) $(CPPFLAGS) $(LLVM_CXXFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/postgamma_state_inventory.o: src/postgamma_state_inventory.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CLANGXX) $(CPPFLAGS) $(LLVM_CXXFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/postgamma_inheritance_inventory.o: src/postgamma_inheritance_inventory.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CLANGXX) $(CPPFLAGS) $(LLVM_CXXFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/postgamma_api_inventory.o: src/postgamma_api_inventory.cpp
	@mkdir -p "$(BUILD_DIR)"
	$(CLANGXX) $(CPPFLAGS) $(LLVM_CXXFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(TARGETS) $(OBJECTS) $(DEPENDENCIES)

-include $(DEPENDENCIES)
