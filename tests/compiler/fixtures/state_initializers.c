int implicit_zero;
int explicit_zero = 0;
_Bool boolean_zero = 0;
int *pointer_zero = (void *) 0;
double float_zero = 0.0;
double negative_zero = -0.0;
long double padded_float_zero = 0.0L;
int array_zero[4] = {0};
int array_nonzero[4] = {0, 1};
struct padded { char c; int i; } record_zero = {0};
union ambiguous { int i; long l; } union_zero = {0};
struct bits { unsigned int b:1; } bitfield_zero = {0};
int nonzero = 7;
static const int constants[] = {17, 19};
static const int *internal_template = &constants[1];
static int *relocated_template = &nonzero;
int weak_zero __attribute__((weak)) = 0;
int asm_zero __asm__("other_spelling") = 0;

int touch_initializers(void)
{
    static int function_zero = 0;
    static int function_nonzero = 9;
    return ++function_zero + function_nonzero + *internal_template +
           *relocated_template;
}
