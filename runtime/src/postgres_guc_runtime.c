/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/*-------------------------------------------------------------------------
 *
 * postgres_guc_runtime.c
 *    PostgreSQL integration for GUC state virtualization.
 *
 * Generated include files provide only version-dependent GUC facts.  The
 * capture, clone, lookup, and binding algorithms remain handwritten here.
 *
 *-------------------------------------------------------------------------
 */

#define POSTGAMMA_POSTGRES_RUNTIME_IMPLEMENTATION
#include "postgres.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "postgamma/instance_runtime.h"
#include "postgamma/postgres_backend_runtime.h"


typedef struct PostgammaGucControlState
{
#define POSTGAMMA_GUC_CONTROL(slot, field, c_type, replacement, initializer) \
	c_type field;
#include "postgamma/guc_control_facts.inc"
#undef POSTGAMMA_GUC_CONTROL
} PostgammaGucControlState;

typedef struct PostgammaBuiltinGucDescriptor
{
	const char *name;
	PostgammaGucSlot slot;
	enum config_type vartype;
	PostgammaGucOwner owner;
} PostgammaBuiltinGucDescriptor;


static PostgammaInstanceContext PostgammaBootstrapInstanceContext;
static PostgammaRoleContext PostgammaBootstrapRoleContext;
static PostgammaExecutionContext PostgammaBootstrapExecutionContext;

static const PostgammaBuiltinGucDescriptor PostgammaBuiltinGucs[] =
{
#define POSTGAMMA_GUC_DESCRIPTOR(name, slot, vartype, storage, owner) \
	{name, slot, vartype, owner},
#include "postgamma/guc_descriptors.inc"
#undef POSTGAMMA_GUC_DESCRIPTOR
};


static const PostgammaBuiltinGucDescriptor *
postgamma_find_descriptor(const char *name)
{
	int			low = 0;
	int			high = lengthof(PostgammaBuiltinGucs) - 1;

	while (low <= high)
	{
		int			middle = low + (high - low) / 2;
		int			comparison = strcmp(name, PostgammaBuiltinGucs[middle].name);

		if (comparison < 0)
			high = middle - 1;
		else if (comparison > 0)
			low = middle + 1;
		else
			return &PostgammaBuiltinGucs[middle];
	}
	return NULL;
}


void *
postgamma_guc_slot_address(PostgammaGucSlot slot)
{
	switch (slot)
	{
#define POSTGAMMA_GUC_DESCRIPTOR(name, descriptor_slot, vartype, storage, owner) \
		case descriptor_slot: \
			return &POSTGAMMA_GUC_VALUE(storage);
#include "postgamma/guc_descriptors.inc"
#undef POSTGAMMA_GUC_DESCRIPTOR
		case POSTGAMMA_GUC_SLOT_COUNT:
			break;
		default:
			break;
	}
	postgamma_runtime_contract_violation();
	return NULL;
}


static void *
postgamma_guc_control_slot_address_internal(PostgammaGucControlState *control,
										PostgammaGucControlSlot slot)
{
	if (control == NULL)
		postgamma_runtime_contract_violation();
	switch (slot)
	{
#define POSTGAMMA_GUC_CONTROL(control_slot, field, c_type, replacement, initializer) \
		case control_slot: \
			return &control->field;
#include "postgamma/guc_control_facts.inc"
#undef POSTGAMMA_GUC_CONTROL
		case POSTGAMMA_GUC_CONTROL_SLOT_COUNT:
			break;
	}
	postgamma_runtime_contract_violation();
	return NULL;
}


void *
postgamma_guc_control_slot_address(PostgammaGucControlSlot slot)
{
	PostgammaExecutionContext *execution = postgamma_execution_context_require();
	PostgammaGucControlState *control = execution->postgres_guc_control;

	return postgamma_guc_control_slot_address_internal(control, slot);
}


static void
postgamma_copy_guc_value(const PostgammaBuiltinGucDescriptor *descriptor,
						 const struct config_generic *source)
{
	void	   *destination = postgamma_guc_slot_address(descriptor->slot);

	if (source->vartype != descriptor->vartype)
		elog(FATAL, "PostgreSQL changed the type of built-in GUC %s",
			 descriptor->name);
	switch (descriptor->vartype)
	{
		case PGC_BOOL:
			*(bool *) destination = *source->_bool.variable;
			break;
		case PGC_INT:
			*(int *) destination = *source->_int.variable;
			break;
		case PGC_REAL:
			*(double *) destination = *source->_real.variable;
			break;
		case PGC_STRING:
			*(char **) destination = *source->_string.variable;
			break;
		case PGC_ENUM:
			*(int *) destination = *source->_enum.variable;
			break;
	}
}


static void
postgamma_rebind_guc_value(const PostgammaBuiltinGucDescriptor *descriptor,
						   struct config_generic *target)
{
	void	   *destination = postgamma_guc_slot_address(descriptor->slot);

	if (target->vartype != descriptor->vartype)
		elog(FATAL, "PostgreSQL changed the type of built-in GUC %s",
			 descriptor->name);
	switch (descriptor->vartype)
	{
		case PGC_BOOL:
			target->_bool.variable = destination;
			break;
		case PGC_INT:
			target->_int.variable = destination;
			break;
		case PGC_REAL:
			target->_real.variable = destination;
			break;
		case PGC_STRING:
			target->_string.variable = destination;
			break;
		case PGC_ENUM:
			target->_enum.variable = destination;
			break;
	}
}


static void *
postgamma_instance_guc_slot_address(
	PostgammaInstanceContext *instance,
	const PostgammaBuiltinGucDescriptor *descriptor)
{
	if (instance == NULL ||
		instance->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		descriptor == NULL)
		postgamma_runtime_contract_violation();
	switch (descriptor->slot)
	{
#define POSTGAMMA_GUC_OWNER_ADDRESS_POSTGAMMA_GUC_OWNER_IMMUTABLE(storage) \
		(&instance->immutable_guc.storage)
#define POSTGAMMA_GUC_OWNER_ADDRESS_POSTGAMMA_GUC_OWNER_INSTANCE(storage) \
		(&instance->instance_guc.storage)
#define POSTGAMMA_GUC_OWNER_ADDRESS_POSTGAMMA_GUC_OWNER_ROLE(storage) NULL
#define POSTGAMMA_GUC_OWNER_ADDRESS_POSTGAMMA_GUC_OWNER_SESSION(storage) NULL
#define POSTGAMMA_GUC_OWNER_ADDRESS(owner, storage) \
		POSTGAMMA_GUC_OWNER_ADDRESS_EXPAND(owner, storage)
#define POSTGAMMA_GUC_OWNER_ADDRESS_EXPAND(owner, storage) \
		POSTGAMMA_GUC_OWNER_ADDRESS_##owner(storage)
#define POSTGAMMA_GUC_DESCRIPTOR(name, slot, vartype, storage, owner) \
		case slot: \
			return POSTGAMMA_GUC_OWNER_ADDRESS(owner, storage);
#include "postgamma/guc_descriptors.inc"
#undef POSTGAMMA_GUC_DESCRIPTOR
#undef POSTGAMMA_GUC_OWNER_ADDRESS_EXPAND
#undef POSTGAMMA_GUC_OWNER_ADDRESS
#undef POSTGAMMA_GUC_OWNER_ADDRESS_POSTGAMMA_GUC_OWNER_SESSION
#undef POSTGAMMA_GUC_OWNER_ADDRESS_POSTGAMMA_GUC_OWNER_ROLE
#undef POSTGAMMA_GUC_OWNER_ADDRESS_POSTGAMMA_GUC_OWNER_INSTANCE
#undef POSTGAMMA_GUC_OWNER_ADDRESS_POSTGAMMA_GUC_OWNER_IMMUTABLE
		case POSTGAMMA_GUC_SLOT_COUNT:
			break;
		default:
			break;
	}
	postgamma_runtime_contract_violation();
	return NULL;
}


static bool
postgamma_guc_slot_matches_target(
	const PostgammaBuiltinGucDescriptor *descriptor,
	const void *source, const struct config_generic *target)
{
	if (descriptor == NULL || source == NULL || target == NULL ||
		descriptor->vartype != target->vartype)
		postgamma_runtime_contract_violation();
	switch (descriptor->vartype)
	{
		case PGC_BOOL:
			return *(const bool *) source == *target->_bool.variable;
		case PGC_INT:
			return *(const int *) source == *target->_int.variable;
		case PGC_REAL:
			return *(const double *) source == *target->_real.variable;
		case PGC_STRING:
		{
			const char *source_value = *(char *const *) source;
			const char *target_value = *target->_string.variable;

			return source_value == target_value ||
				(source_value != NULL && target_value != NULL &&
				 strcmp(source_value, target_value) == 0);
		}
		case PGC_ENUM:
			return *(const int *) source == *target->_enum.variable;
	}
	postgamma_runtime_contract_violation();
	return false;
}


static bool
postgamma_guc_is_instance_shared(
	const PostgammaBuiltinGucDescriptor *descriptor)
{
	if (descriptor == NULL)
		postgamma_runtime_contract_violation();
	return descriptor->owner == POSTGAMMA_GUC_OWNER_IMMUTABLE ||
		descriptor->owner == POSTGAMMA_GUC_OWNER_INSTANCE;
}


static int
postgamma_builtin_guc_count(void)
{
	int			count = 0;

	while (ConfigureNames[count].name != NULL)
		count++;
	return count;
}


/*
 * PostgreSQL omits PGC_POSTMASTER values from GUC snapshots because a child
 * process inherits both built-in and extension storage from the postmaster.
 * A thread role starts with freshly initialized role storage, so every
 * non-default PGC_POSTMASTER value must travel in PostgreSQL's GUC snapshot.
 * In addition to the value, SerializeGUCState() preserves source precedence,
 * source file, source line, context, and role.  Copying only backing storage
 * would lose that provenance and make pg_settings report every inherited
 * postmaster setting as an override.
 *
 * Only the postmaster's thread-launch snapshot needs this extension.  A
 * backend role has already restored that snapshot before it can launch or
 * become a parallel worker.  Keep PostgreSQL's normal skip rule in those
 * roles: can_skip_gucvar() also controls RestoreGUCState()'s reset-to-default
 * pass.  Resetting an active PGC_POSTMASTER setting such as shared_buffers
 * would let GUC check hooks access shared memory with its compiled-in size
 * instead of the size actually allocated by the postmaster.
 */
bool
postgamma_guc_state_requires_transfer(const struct config_generic *guc)
{
	if (guc == NULL)
		postgamma_runtime_contract_violation();
	return !postgamma_in_backend_thread() &&
		guc->context == PGC_POSTMASTER &&
		guc->source != PGC_S_DEFAULT;
}


void
postgamma_initialize_builtin_guc_values(void)
{
	int			count = postgamma_builtin_guc_count();
	int			index;

	if (count != lengthof(PostgammaBuiltinGucs))
		elog(FATAL, "PostGamma catalog has %zu built-in GUCs, PostgreSQL compiled %d",
			 lengthof(PostgammaBuiltinGucs), count);
	for (index = 0; index < count; index++)
	{
		const PostgammaBuiltinGucDescriptor *descriptor =
			postgamma_find_descriptor(ConfigureNames[index].name);

		if (descriptor == NULL)
			elog(FATAL, "PostGamma catalog does not contain built-in GUC %s",
				 ConfigureNames[index].name);
		postgamma_copy_guc_value(descriptor, &ConfigureNames[index]);
	}
}


static PostgammaGucControlState *
postgamma_create_guc_control(void)
{
	PostgammaGucControlState *control = calloc(1, sizeof(*control));

	if (control == NULL)
		elog(FATAL, "out of memory creating PostGamma GUC control state");

#define POSTGAMMA_GUC_CONTROL_INIT_zero(control, field) ((void) 0)
#define POSTGAMMA_GUC_CONTROL_INIT_dlist(control, field) dlist_init(&(control)->field)
#define POSTGAMMA_GUC_CONTROL_INIT_slist(control, field) slist_init(&(control)->field)
#define POSTGAMMA_GUC_CONTROL_INIT_configure_names(control, field) ((void) 0)
#define POSTGAMMA_GUC_CONTROL(slot, field, c_type, replacement, initializer) \
	POSTGAMMA_GUC_CONTROL_INIT_##initializer(control, field);
#include "postgamma/guc_control_facts.inc"
#undef POSTGAMMA_GUC_CONTROL
#undef POSTGAMMA_GUC_CONTROL_INIT_configure_names
#undef POSTGAMMA_GUC_CONTROL_INIT_slist
#undef POSTGAMMA_GUC_CONTROL_INIT_dlist
#undef POSTGAMMA_GUC_CONTROL_INIT_zero

	return control;
}


void
postgamma_server_execution_context_bootstrap(void)
{
	PostgammaInstanceRuntimeOptions options =
		POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
	PostgammaInstanceRuntime *runtime;
	PostgammaExecutionContext *previous;
	int			status;

	if (postgamma_execution_context_current() != NULL)
		elog(FATAL, "PostGamma execution context is already bound at bootstrap");
	options.profile = POSTGAMMA_RUNTIME_PROFILE_THREADED_SERVER;
	options.wake_notification =
		postgamma_threaded_server_completion_notify;
	status = postgamma_instance_runtime_create(&runtime, &options);
	if (status != 0)
		elog(FATAL, "could not create PostGamma instance runtime: %s",
			 strerror(status));
	status = postgamma_instance_context_init(&PostgammaBootstrapInstanceContext);
	if (status != 0)
		elog(FATAL, "could not create PostGamma instance context: %s",
			 strerror(status));
	postgamma_instance_context_attach_runtime(
		&PostgammaBootstrapInstanceContext, runtime);
	status = postgamma_role_context_init(&PostgammaBootstrapRoleContext,
								   &PostgammaBootstrapInstanceContext);
	if (status != 0)
		elog(FATAL, "could not create PostGamma role context: %s",
			 strerror(status));
	status = postgamma_execution_context_init(&PostgammaBootstrapExecutionContext,
									 &PostgammaBootstrapInstanceContext,
									 &PostgammaBootstrapRoleContext);
	if (status != 0)
		elog(FATAL, "could not create PostGamma execution context: %s",
			 strerror(status));
	previous = postgamma_execution_context_bind(&PostgammaBootstrapExecutionContext);
	if (previous != NULL)
		elog(FATAL, "unexpected previous PostGamma execution context");
	postgamma_initialize_builtin_guc_values();
}


void
postgamma_bind_builtin_guc_variables(void)
{
	PostgammaExecutionContext *execution = postgamma_execution_context_require();
	PostgammaGucControlState *control = execution->postgres_guc_control;
	struct config_generic **configure_names;
	int			count;
	int			index;

	if (control != NULL)
	{
		configure_names = postgamma_guc_control_slot_address_internal(
			control, POSTGAMMA_GUC_CONFIGURE_NAMES_SLOT);
		if (*configure_names == NULL)
			elog(FATAL, "partially initialized PostGamma GUC control state");
		return;
	}
	control = postgamma_create_guc_control();
	execution->postgres_guc_control = control;
	configure_names = postgamma_guc_control_slot_address_internal(
		control, POSTGAMMA_GUC_CONFIGURE_NAMES_SLOT);
	count = postgamma_builtin_guc_count();
	if (count != lengthof(PostgammaBuiltinGucs))
		elog(FATAL, "PostGamma catalog has %zu built-in GUCs, PostgreSQL compiled %d",
			 lengthof(PostgammaBuiltinGucs), count);
	*configure_names = malloc((count + 1) * sizeof(ConfigureNames[0]));
	if (*configure_names == NULL)
		elog(FATAL, "out of memory cloning PostgreSQL built-in GUC table");
	memcpy(*configure_names, ConfigureNames,
		   (count + 1) * sizeof(ConfigureNames[0]));
	for (index = 0; index < count; index++)
	{
		const PostgammaBuiltinGucDescriptor *descriptor =
			postgamma_find_descriptor(ConfigureNames[index].name);

		if (descriptor == NULL)
			elog(FATAL, "PostGamma catalog does not contain built-in GUC %s",
				 ConfigureNames[index].name);
		postgamma_rebind_guc_value(descriptor, &(*configure_names)[index]);
	}
}


/*
 * Seed the backend-owned GUC metadata shadow from the live server instance.
 * PostgreSQL's native GUC snapshot owns the metadata for every shippable
 * option, including its source and source context.  PostGamma extends that
 * snapshot to cover non-default PGC_POSTMASTER options.  PGC_INTERNAL values
 * still use PostgreSQL's special initialization paths and are not serialized,
 * so fill only that native inheritance gap here.
 *
 * Skip values already installed by InitializeGUCOptions.  Besides avoiding
 * redundant hook side effects, this is required for groups such as the
 * mutually exclusive recovery targets, whose empty-value assign hooks share
 * one role-local discriminator.  PGC_INTERNAL values retain dynamic-default
 * semantics.
 */
void
postgamma_seed_inherited_guc_shadow(PostgammaInstanceContext *shared_instance)
{
	PostgammaExecutionContext *execution = postgamma_execution_context_require();
	PostgammaGucControlState *control = execution->postgres_guc_control;
	struct config_generic **configure_names;
	int			count;
	int			index;

	if (control == NULL || shared_instance == NULL ||
		shared_instance->magic != POSTGAMMA_INSTANCE_CONTEXT_MAGIC ||
		execution->instance == shared_instance)
		postgamma_runtime_contract_violation();
	configure_names = postgamma_guc_control_slot_address_internal(
		control, POSTGAMMA_GUC_CONFIGURE_NAMES_SLOT);
	if (*configure_names == NULL)
		postgamma_runtime_contract_violation();
	count = postgamma_builtin_guc_count();
	if (count != lengthof(PostgammaBuiltinGucs))
		elog(FATAL, "PostGamma catalog has %zu built-in GUCs, PostgreSQL compiled %d",
			 lengthof(PostgammaBuiltinGucs), count);
	for (index = 0; index < count; index++)
	{
		char		value_buffer[64];
		const char *value;
		GucSource	value_source;
		int			set_result;
		void	   *source;
		struct config_generic *target = &(*configure_names)[index];
		const PostgammaBuiltinGucDescriptor *descriptor =
			postgamma_find_descriptor(target->name);

		if (descriptor == NULL)
			elog(FATAL, "PostGamma catalog does not contain built-in GUC %s",
				 target->name);
		if (!postgamma_guc_is_instance_shared(descriptor) ||
			target->context != PGC_INTERNAL)
			continue;

		source = postgamma_instance_guc_slot_address(
			shared_instance, descriptor);
		if (source == NULL)
			postgamma_runtime_contract_violation();
		if (postgamma_guc_slot_matches_target(descriptor, source, target))
			continue;
		switch (descriptor->vartype)
		{
			case PGC_BOOL:
				value = *(bool *) source ? "true" : "false";
				break;
			case PGC_INT:
				snprintf(value_buffer, sizeof(value_buffer), "%d",
						 *(int *) source);
				value = value_buffer;
				break;
			case PGC_REAL:
				snprintf(value_buffer, sizeof(value_buffer), "%.*g",
						 DBL_DECIMAL_DIG, *(double *) source);
				value = value_buffer;
				break;
			case PGC_STRING:
				value = *(char **) source;
				break;
			case PGC_ENUM:
				value = config_enum_lookup_by_value(target, *(int *) source);
				break;
			default:
				postgamma_runtime_contract_violation();
				value = NULL;
				break;
		}
		if (value == NULL)
			continue;
		value_source = PGC_S_DYNAMIC_DEFAULT;
		set_result = set_config_option(target->name, value, target->context,
									   value_source, GUC_ACTION_SET,
									   true, ERROR, false);
		if (set_result <= 0 ||
			!postgamma_guc_slot_matches_target(descriptor, source, target))
			elog(FATAL, "could not seed inherited GUC %s", target->name);
	}
}


void
postgamma_destroy_builtin_guc_variables(PostgammaExecutionContext *execution)
{
	PostgammaGucControlState *control;
	struct config_generic **configure_names;

	if (execution == NULL ||
		execution->magic != POSTGAMMA_EXECUTION_CONTEXT_MAGIC)
		postgamma_runtime_contract_violation();
	control = execution->postgres_guc_control;
	if (control == NULL)
		return;
	configure_names = postgamma_guc_control_slot_address_internal(
		control, POSTGAMMA_GUC_CONFIGURE_NAMES_SLOT);
	free(*configure_names);
	*configure_names = NULL;
	free(control);
	execution->postgres_guc_control = NULL;
}
