/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
/* Test link only: delay observing one real supervisor ticket's completion.
 * Native tests separately keep the supervisor callback itself in flight. */
static bool control_gate_armed;
static PostgammaSupervisorTicket *control_gate_ticket;
extern int __real_postgamma_supervisor_ticket_wait(
    PostgammaSupervisorTicket *, uint64_t, int *);
int __wrap_postgamma_supervisor_ticket_wait(
    PostgammaSupervisorTicket *ticket, uint64_t deadline, int *status)
{
    if (control_gate_armed && !control_gate_ticket) control_gate_ticket = ticket;
    if (control_gate_armed && control_gate_ticket == ticket) {
        uint64_t stop = postgamma_monotonic_now_ns() + UINT64_C(5000000000);
        if (deadline < stop) stop = deadline;
        // A synchronous caller occupies the driver; a poll returns immediately.
        while (postgamma_monotonic_now_ns() < stop) usleep(1000);
        return ETIMEDOUT;
    }
    return __real_postgamma_supervisor_ticket_wait(ticket, deadline, status);
}
