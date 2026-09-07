SHELL := /bin/bash

ROOT := $(abspath ..)
BUILD_DIR ?= $(ROOT)/build/embedded-c
override BUILD_DIR := $(abspath $(BUILD_DIR))
CC ?= cc
AR ?= ar
CPPFLAGS ?=
CFLAGS ?= -O2

PRIVATE_CPPFLAGS := -I$(CURDIR)/include -I$(ROOT)/runtime/include
PRIVATE_CFLAGS := -std=c11 -Wall -Wextra -Werror -Wpedantic
PRIVATE_THREAD_FLAGS := -pthread
PRIVATE_HEADERS := $(sort $(wildcard \
	include/postgamma/private/*.h \
	$(ROOT)/runtime/include/postgamma/*.h))
SPECIALIZED_SOURCES := src/libpq_memory_adapter.c src/postgres_bootstrap_bridge.c \
	src/postgamma.c src/postgamma_protocol.c src/postgamma_copy.c \
	src/postgamma_events.c src/postgamma_arrow.c src/postgamma_management.c \
	src/postgamma_logical_management.c \
	src/postgres_initdb_host.c src/postgres_logical_tool_host.c \
	src/postgres_static_modules.c \
	src/bootstrap_probe.c
SOURCES := $(filter-out $(SPECIALIZED_SOURCES),$(sort $(wildcard src/*.c)))
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
LIBRARY := $(BUILD_DIR)/libpostgamma-embedded-c.a
TEST_SOURCES := $(sort $(wildcard tests/test_*.c))
TEST_PROGRAMS := $(patsubst tests/%.c,$(BUILD_DIR)/tests/%,$(TEST_SOURCES))
SUPERVISOR_ASAN := $(BUILD_DIR)/sanitizers/test-supervisor-asan
SUPERVISOR_TSAN := $(BUILD_DIR)/sanitizers/test-supervisor-tsan
KERNEL_ADAPTER_ASAN := $(BUILD_DIR)/sanitizers/test-kernel-adapter-asan
KERNEL_ADAPTER_TSAN := $(BUILD_DIR)/sanitizers/test-kernel-adapter-tsan
DATA_LOCK_ASAN := $(BUILD_DIR)/sanitizers/test-data-directory-lock-asan
DATA_LOCK_TSAN := $(BUILD_DIR)/sanitizers/test-data-directory-lock-tsan

$(OBJECTS) $(TEST_PROGRAMS) $(SUPERVISOR_ASAN) $(SUPERVISOR_TSAN) \
	$(KERNEL_ADAPTER_ASAN) $(KERNEL_ADAPTER_TSAN) \
	$(DATA_LOCK_ASAN) $(DATA_LOCK_TSAN): $(PRIVATE_HEADERS) Makefile

.DEFAULT_GOAL := all
.PHONY: all check sanitizer-check

all: $(if $(SOURCES),$(LIBRARY))

check: all $(TEST_PROGRAMS)
	@set -e; for test_program in $(TEST_PROGRAMS); do "$$test_program"; done

sanitizer-check: $(SUPERVISOR_ASAN) $(SUPERVISOR_TSAN) \
		$(KERNEL_ADAPTER_ASAN) $(KERNEL_ADAPTER_TSAN) \
		$(DATA_LOCK_ASAN) $(DATA_LOCK_TSAN)
	@ASAN_OPTIONS='detect_leaks=1:halt_on_error=1' \
		UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' \
		"$(SUPERVISOR_ASAN)"
	@ASAN_OPTIONS='detect_leaks=1:halt_on_error=1' \
		UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' \
		"$(KERNEL_ADAPTER_ASAN)"
	@ASAN_OPTIONS='detect_leaks=1:halt_on_error=1' \
		UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' \
		"$(DATA_LOCK_ASAN)"
	@setarch "$$(uname -m)" -R env TSAN_OPTIONS='halt_on_error=1' \
		"$(SUPERVISOR_TSAN)"
	@setarch "$$(uname -m)" -R env TSAN_OPTIONS='halt_on_error=1' \
		"$(KERNEL_ADAPTER_TSAN)"
	@setarch "$$(uname -m)" -R env TSAN_OPTIONS='halt_on_error=1' \
		"$(DATA_LOCK_TSAN)"

$(LIBRARY): $(OBJECTS)
	@mkdir -p "$(@D)"
	@$(AR) crs "$@" $(OBJECTS)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(CFLAGS) $(PRIVATE_CFLAGS) \
		$(PRIVATE_THREAD_FLAGS) \
		-c "$<" -o "$@"

$(BUILD_DIR)/tests/test_supervisor: tests/test_supervisor.c $(LIBRARY) \
		$(ROOT)/runtime/src/thread_runtime.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(CFLAGS) $(PRIVATE_CFLAGS) \
		"$<" $(LIBRARY) $(ROOT)/runtime/src/thread_runtime.c \
		$(PRIVATE_THREAD_FLAGS) -o "$@"

$(BUILD_DIR)/tests/test_kernel_supervisor_adapter: \
		tests/test_kernel_supervisor_adapter.c $(LIBRARY) \
		$(ROOT)/runtime/src/thread_runtime.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(CFLAGS) $(PRIVATE_CFLAGS) \
		"$<" $(LIBRARY) $(ROOT)/runtime/src/thread_runtime.c \
		$(PRIVATE_THREAD_FLAGS) -o "$@"

$(SUPERVISOR_ASAN): tests/test_supervisor.c src/supervisor.c \
		$(ROOT)/runtime/src/thread_runtime.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(PRIVATE_CFLAGS) \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		$(filter %.c,$^) $(PRIVATE_THREAD_FLAGS) -o "$@"

$(SUPERVISOR_TSAN): tests/test_supervisor.c src/supervisor.c \
		$(ROOT)/runtime/src/thread_runtime.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(PRIVATE_CFLAGS) \
		-fsanitize=thread -fno-omit-frame-pointer -fPIE -pie \
		$(filter %.c,$^) $(PRIVATE_THREAD_FLAGS) -o "$@"

$(KERNEL_ADAPTER_ASAN): tests/test_kernel_supervisor_adapter.c \
		src/kernel_supervisor_adapter.c src/supervisor.c \
		$(ROOT)/runtime/src/thread_runtime.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(PRIVATE_CFLAGS) \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		$(filter %.c,$^) $(PRIVATE_THREAD_FLAGS) -o "$@"

$(KERNEL_ADAPTER_TSAN): tests/test_kernel_supervisor_adapter.c \
		src/kernel_supervisor_adapter.c src/supervisor.c \
		$(ROOT)/runtime/src/thread_runtime.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(PRIVATE_CFLAGS) \
		-fsanitize=thread -fno-omit-frame-pointer -fPIE -pie \
		$(filter %.c,$^) $(PRIVATE_THREAD_FLAGS) -o "$@"

$(DATA_LOCK_ASAN): tests/test_data_directory_lock.c src/data_directory_lock.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(PRIVATE_CFLAGS) \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		$(filter %.c,$^) $(PRIVATE_THREAD_FLAGS) -o "$@"

$(DATA_LOCK_TSAN): tests/test_data_directory_lock.c src/data_directory_lock.c
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(PRIVATE_CFLAGS) \
		-fsanitize=thread -fno-omit-frame-pointer -fPIE -pie \
		$(filter %.c,$^) $(PRIVATE_THREAD_FLAGS) -o "$@"

$(BUILD_DIR)/tests/%: tests/%.c $(if $(SOURCES),$(LIBRARY))
	@mkdir -p "$(@D)"
	@$(CC) $(CPPFLAGS) $(PRIVATE_CPPFLAGS) $(CFLAGS) $(PRIVATE_CFLAGS) \
		"$<" $(if $(SOURCES),$(LIBRARY)) \
		$(PRIVATE_THREAD_FLAGS) -o "$@"
