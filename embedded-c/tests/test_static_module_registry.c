#include "postgamma/private/static_module_registry.h"

#include <stddef.h>

static void
fixture_alpha(void)
{
}

static void
fixture_beta(void)
{
}

int
main(void)
{
	static const PostgammaStaticModuleSymbol symbols[] = {
		{"alpha", fixture_alpha},
		{"beta", fixture_beta}
	};
	static const PostgammaStaticModuleDefinition modules[] = {
		POSTGAMMA_STATIC_MODULE_INITIALIZER("fixture", symbols)
	};
	static const PostgammaStaticModuleRegistry registry =
		POSTGAMMA_STATIC_REGISTRY_INITIALIZER(modules);
	static const PostgammaStaticModuleSymbol unsorted_symbols[] = {
		{"beta", fixture_beta},
		{"alpha", fixture_alpha}
	};
	static const PostgammaStaticModuleDefinition unsorted_modules[] = {
		POSTGAMMA_STATIC_MODULE_INITIALIZER("fixture", unsorted_symbols)
	};
	static const PostgammaStaticModuleRegistry unsorted_registry =
		POSTGAMMA_STATIC_REGISTRY_INITIALIZER(unsorted_modules);
	PostgammaStaticModuleHandle module;
	PostgammaStaticModuleHandle missing;

	if (!postgamma_static_module_open(&registry, "fixture", &module) ||
		postgamma_static_module_symbol(&module, "alpha") != fixture_alpha ||
		postgamma_static_module_symbol(&module, "beta") != fixture_beta ||
		postgamma_static_module_symbol(&module, "missing") != NULL ||
		postgamma_static_module_open(&registry, "missing", &missing) ||
		postgamma_static_module_dynamic_load_attempts(&registry) != 0 ||
		!postgamma_static_module_close(&module) ||
		postgamma_static_module_symbol(&module, "alpha") != NULL ||
		postgamma_static_module_open(&unsorted_registry, "fixture", &module))
		return 1;
	return 0;
}
