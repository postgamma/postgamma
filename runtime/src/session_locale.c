/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "postgamma/session_locale.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int postgamma_locale_category_index(int category);
static int postgamma_locale_category_mask(int category);
static const char *postgamma_effective_locale_name(
	int category, const char *locale);
static void postgamma_initialize_locale_names(
	PostgammaSessionLocale *state);

static void
copy_locale_name(char *destination, const char *source, size_t capacity)
{
    size_t length = strlen(source);
    if (length >= capacity)
        length = capacity - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

char *
postgamma_session_locale_set(PostgammaSessionLocale *state, int category, const char *locale)
{
	locale_t	base;
	locale_t	updated;
	locale_t	old_locale;
	const char *resolved;
	int			category_index;
	int			category_mask;
	int			saved_errno;

	category_index = postgamma_locale_category_index(category);
	category_mask = postgamma_locale_category_mask(category);
	if (category_index < 0 || category_mask == 0)
	{
		errno = EINVAL;
		return NULL;
	}
	postgamma_initialize_locale_names(state);
	if (locale == NULL)
		return state->locale_names[category_index];

	old_locale = state->session_locale;
	base = duplocale(old_locale != (locale_t) 0 ?
					 old_locale : LC_GLOBAL_LOCALE);
	if (base == (locale_t) 0)
		return NULL;
	updated = newlocale(category_mask, locale, base);
	if (updated == (locale_t) 0)
	{
		saved_errno = errno;
		freelocale(base);
		errno = saved_errno;
		return NULL;
	}
	if (uselocale(updated) == (locale_t) 0)
	{
		saved_errno = errno;
		freelocale(updated);
		errno = saved_errno;
		return NULL;
	}
	state->session_locale = updated;
	if (old_locale != (locale_t) 0)
		freelocale(old_locale);

	resolved = postgamma_effective_locale_name(category, locale);
	copy_locale_name(state->locale_names[category_index], resolved,
			sizeof(state->locale_names[category_index]));
	return state->locale_names[category_index];
}

int
postgamma_session_locale_bind(PostgammaSessionLocale *state, locale_t *previous)
{
    *previous = uselocale(LC_GLOBAL_LOCALE);
    if (*previous == (locale_t) 0)
        return errno;
    if (state->session_locale != (locale_t) 0 &&
        uselocale(state->session_locale) == (locale_t) 0)
        return errno;
    return 0;
}

int
postgamma_session_locale_unbind(locale_t *previous)
{
    if (uselocale(*previous) == (locale_t) 0)
        return errno;
    *previous = (locale_t) 0;
    return 0;
}

void
postgamma_session_locale_destroy(PostgammaSessionLocale *state)
{
    if (state->session_locale != (locale_t) 0)
    {
        freelocale(state->session_locale);
        state->session_locale = (locale_t) 0;
    }
}

static int
postgamma_locale_category_index(int category)
{
	switch (category)
	{
		case LC_COLLATE:
			return POSTGAMMA_LOCALE_COLLATE;
		case LC_CTYPE:
			return POSTGAMMA_LOCALE_CTYPE;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			return POSTGAMMA_LOCALE_MESSAGES;
#endif
		case LC_MONETARY:
			return POSTGAMMA_LOCALE_MONETARY;
		case LC_NUMERIC:
			return POSTGAMMA_LOCALE_NUMERIC;
		case LC_TIME:
			return POSTGAMMA_LOCALE_TIME;
	}
	return -1;
}


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
}


static void
postgamma_initialize_locale_names(PostgammaSessionLocale *state)
{
	static const int categories[POSTGAMMA_LOCALE_CATEGORY_COUNT] =
	{
		LC_COLLATE,
		LC_CTYPE,
#ifdef LC_MESSAGES
		LC_MESSAGES,
#else
		-1,
#endif
		LC_MONETARY,
		LC_NUMERIC,
		LC_TIME
	};

	if (state->locale_names_initialized)
		return;
	for (int index = 0; index < POSTGAMMA_LOCALE_CATEGORY_COUNT; index++)
	{
		const char *name = categories[index] < 0 ? "C" :
			postgamma_effective_locale_name(categories[index], "");

		copy_locale_name(state->locale_names[index], name,
				sizeof(state->locale_names[index]));
	}
	state->locale_names_initialized = true;
}
