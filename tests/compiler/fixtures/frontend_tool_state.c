int frontend_library_state;
static int frontend_tool_buffer;


int
touch_frontend_tool_state(int value)
{
	frontend_library_state = value;
	frontend_tool_buffer += value;
	return frontend_library_state + frontend_tool_buffer;
}
