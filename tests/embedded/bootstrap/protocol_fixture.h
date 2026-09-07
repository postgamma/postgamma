#ifndef POSTGAMMA_BOOTSTRAP_PROTOCOL_FIXTURE_H
#define POSTGAMMA_BOOTSTRAP_PROTOCOL_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "postgamma/private/memory_transport.h"

typedef struct PostgammaBootstrapProtocolFixture PostgammaBootstrapProtocolFixture;

typedef struct PostgammaBootstrapProtocolFixtureTelemetry
{
	unsigned int startup_packets;
	unsigned int query_packets;
	unsigned int copy_packets;
	unsigned int read_retries;
	unsigned int write_retries;
	size_t		copy_bytes_before_response;
	size_t		copy_bytes_received;
	bool		startup_parameters_valid;
	bool		response_preceded_copy_completion;
	bool		simultaneous_queue_saturation;
	bool		frontend_half_close_observed;
	char		detail[128];
} PostgammaBootstrapProtocolFixtureTelemetry;

int postgamma_bootstrap_protocol_fixture_start(
	PostgammaMemoryEndpoint **backend_endpoint,
	uint64_t generation,
	PostgammaBootstrapProtocolFixture **fixture);
int postgamma_bootstrap_protocol_fixture_join(
	PostgammaBootstrapProtocolFixture **fixture,
	PostgammaBootstrapProtocolFixtureTelemetry *telemetry);

#endif
