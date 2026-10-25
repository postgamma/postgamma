extern int runtime_value();
int dynamic_initializer = runtime_value();
struct member { int field; };
int member::*member_pointer = nullptr;
