typedef struct BackendParameters
{
	int copied_external;
	int copied_internal;
} BackendParameters;

int inherited_external;
static int inherited_internal;
const int immutable_global = 7;

static void
save_backend_variables(BackendParameters *parameters, int local_value)
{
	parameters->copied_external = inherited_external + local_value;
	parameters->copied_internal = inherited_internal + immutable_global;
}

int
exercise_inheritance_contract(void)
{
	BackendParameters parameters;

	save_backend_variables(&parameters, 3);
	return parameters.copied_external + parameters.copied_internal;
}
