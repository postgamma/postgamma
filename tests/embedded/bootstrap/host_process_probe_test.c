#include "tests/embedded/bootstrap/host_process_probe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


static int
check_fixture(const char *content, int expected_status,
			  bool expected_observed, mode_t expected_umask)
{
	char	path[] = "/tmp/postgamma-host-status-XXXXXX";
	FILE   *stream;
	mode_t	umask_value = 0777;
	bool	observed = true;
	int		descriptor;
	int		status;

	descriptor = mkstemp(path);
	if (descriptor < 0)
		return 1;
	stream = fdopen(descriptor, "w");
	if (stream == NULL)
	{
		close(descriptor);
		unlink(path);
		return 2;
	}
	if (fputs(content, stream) == EOF)
	{
		fclose(stream);
		unlink(path);
		return 3;
	}
	if (fclose(stream) != 0)
	{
		unlink(path);
		return 3;
	}
	status = postgamma_bootstrap_read_status_umask(
		path, &umask_value, &observed);
	if (unlink(path) != 0)
		return 4;
	return status == expected_status && observed == expected_observed &&
		umask_value == expected_umask ? 0 : 5;
}


int
main(void)
{
	int	status;

	status = check_fixture(
		"Name:\tlegacy-kernel\nVmSize:\t100 kB\nVmRSS:\t10 kB\n",
		0, false, 0);
	if (status != 0)
		return status;
	status = check_fixture("Name:\tmodern-kernel\nUmask:\t0027\n",
		0, true, 0027);
	if (status != 0)
		return status + 10;
	status = check_fixture("Name:\tinvalid-kernel\nUmask:\t1000\n",
		EPROTO, false, 0);
	return status == 0 ? 0 : status + 20;
}
