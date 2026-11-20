/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_LOCALE_RUNTIME_H
#define POSTGAMMA_LOCALE_RUNTIME_H

#include <locale.h>
#include <stdlib.h>

static int
postgamma_locale_category_mask(int category)
{
	switch (category)
	{
		case LC_COLLATE:
			return LC_COLLATE_MASK;
		case LC_CTYPE:
			return LC_CTYPE_MASK;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			return LC_MESSAGES_MASK;
#endif
		case LC_MONETARY:
			return LC_MONETARY_MASK;
		case LC_NUMERIC:
			return LC_NUMERIC_MASK;
		case LC_TIME:
			return LC_TIME_MASK;
	}
	return 0;
}

static const char *
postgamma_effective_locale_name(int category, const char *locale)
{
#ifdef __EMSCRIPTEN__
	(void) category;
	return locale != NULL && locale[0] != '\0' ? locale : "C";
#else
	const char *category_environment = NULL;
	const char *result;

	if (locale != NULL && locale[0] != '\0')
		return locale;
	result = getenv("LC_ALL");
	if (result != NULL && result[0] != '\0')
		return result;
	switch (category)
	{
		case LC_COLLATE:
			category_environment = "LC_COLLATE";
			break;
		case LC_CTYPE:
			category_environment = "LC_CTYPE";
			break;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			category_environment = "LC_MESSAGES";
			break;
#endif
		case LC_MONETARY:
			category_environment = "LC_MONETARY";
			break;
		case LC_NUMERIC:
			category_environment = "LC_NUMERIC";
			break;
		case LC_TIME:
			category_environment = "LC_TIME";
			break;
	}
	if (category_environment != NULL)
	{
		result = getenv(category_environment);
		if (result != NULL && result[0] != '\0')
			return result;
	}
	result = getenv("LANG");
	if (result != NULL && result[0] != '\0')
		return result;
	return "C";
#endif
}

#endif
