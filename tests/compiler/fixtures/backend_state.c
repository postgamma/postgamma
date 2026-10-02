int external_state = 1;
static int internal_state;
static int *dependent_state = &internal_state;
static struct { int value; } anonymous_state;
const int immutable_state = 2;


int
touch_state(void)
{
	static int function_state = 3;

	internal_state += external_state;
	*dependent_state = function_state++;
	return internal_state;
}
