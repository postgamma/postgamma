/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_PRIVATE_ERROR_STATUS_H
#define POSTGAMMA_PRIVATE_ERROR_STATUS_H

#include "postgamma/postgamma.h"
#include <errno.h>

static pgm_status
status_from_errno(int status)
{
	switch (status)
	{
		case 0:
			return PGM_STATUS_OK;
		case EINVAL:
			return PGM_STATUS_INVALID_ARGUMENT;
		case EBUSY:
		case EAGAIN:
		case EALREADY:
			return PGM_STATUS_BUSY;
		case ETIMEDOUT:
			return PGM_STATUS_TIMEOUT;
		case ECANCELED:
		case EINTR:
			return PGM_STATUS_CANCELED;
		case ENOMEM:
			return PGM_STATUS_OUT_OF_MEMORY;
		case ENOTSUP:
			return PGM_STATUS_UNSUPPORTED;
		case ESTALE:
			return PGM_STATUS_VERSION_MISMATCH;
		case EIO:
		case ENOENT:
		case EACCES:
		case EPERM:
			return PGM_STATUS_IO_ERROR;
	}
	return PGM_STATUS_INTERNAL_ERROR;
}

#endif
