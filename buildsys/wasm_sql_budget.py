"""Validate a complete browser SQL run and measure its reviewed resource profile."""
from __future__ import annotations
import json
from pathlib import Path
import re
from wasm_toolchain import WasmError, write_json

SCENARIOS = ('abi', 'bindings', 'concurrency', 'errors', 'results', 'lifecycle', 'disconnect-timeout', 'close-audit', 'open-failed',
             'fatal-trap', 'fatal-abort', 'fatal-opfs-trap', 'fatal-opfs-abort', 'budget')


def generate_policy(root: Path, output: Path, policy: dict) -> None:
    limits = policy['sql_limits']
    if limits['maxConnections'] != policy['initial_max_live_sessions']:
        raise WasmError('SQL admission differs from the session budget')
    if any(type(n) is not int or not 0 < n <= 2**31 for n in limits.values()):
        raise WasmError('SQL limits must be positive integers')
    header = '/* Generated from the selected SQL resource policy. */\n'
    for name, value in limits.items():
        macro = re.sub(r'([a-z])([A-Z])', r'\1_\2', name).upper()
        header += f'#define PGW_{macro} {value}U\n'
    (output / 'wasm-sql-policy.h').write_text(header)
    module = output / 'site/src/protocol/limits.js'
    module.parent.mkdir(parents=True, exist_ok=True)
    module.write_text('export const limits = Object.freeze(' + json.dumps(limits, sort_keys=True) + ');\n')


def summarize(policy: dict, layout: dict, browsers: dict, mode: str) -> dict:
    if mode not in ("debug", "release"):
        raise WasmError("unsupported SQL acceptance mode")
    if set(browsers) != set(SCENARIOS):
        raise WasmError('resource acceptance requires every SQL scenario from the same build')
    modules = []
    for scenario, browser in browsers.items():
        probe = browser.get('probe', {})
        if browser.get('status') != 'passed' or probe.get('scenario') != scenario or not probe.get('checks'):
            raise WasmError('missing passing SQL scenario: ' + scenario)
        modules.extend(probe['metrics'])
        for event in browser.get('events', []):
            if event.get('kind') in ('fatal', 'lifecycle'):
                modules.append(event['recreated'])
    pool = sum(role['count'] for role in policy['thread_roles'])
    healthy, fatal, stacks, roles = [], [], [], []
    for module in modules:
        if module.get('samples', 0) < 2 or not 1 <= module['peakThreads'] <= pool:
            raise WasmError('missing lifecycle/thread measurements')
        if module['peakMemory'] > policy['maximum_memory_bytes']:
            raise WasmError('linear memory exceeds the profile')
        if module['peakQueuedBytes'] > policy['sql_limits']['workerQueueBytes'] or module['peakPending'] > policy['sql_limits']['maxPendingCommands']:
            raise WasmError('Worker queues exceed the profile')
        if module.get('peakHostQueuedBytes', 0) > policy['sql_limits']['connectionQueueBytes'] * policy['initial_max_live_sessions']:
            raise WasmError('connection queues exceed the profile')
        if module.get('terminationRequested'):
            if module.get('runningWorkers') != 0 or module.get('interruptedWorkers', 0) <= 0:
                raise WasmError('missing fatal Worker termination evidence')
            fatal.append(module)
            continue
        if module.get('returnedWorkers') != pool or module.get('runningWorkers') != 0 or module.get('droppedLogs') != 0:
            raise WasmError('incomplete normal close or lifecycle diagnostics')
        if module.get('resources') != {'locks': 0, 'shmem': 0, 'dsm': 0, 'pipes': 0}:
            raise WasmError('native resources remain after close')
        local_stacks = []
        for line in module['output']:
            if 'POSTGAMMA_STACK ' in line:
                row = dict(item.split('=', 1) for item in line.split('POSTGAMMA_STACK ', 1)[1].split())
                for key in ('observation_failures', 'depth_validation_failures', 'accounting_failures', 'active_reservation_bytes'):
                    if row.get(key) != '0': raise WasmError('stack accounting failed: ' + key)
                if row['class'] in ('dedicated', 'client') and int(row['depth_validations']) == 0:
                    raise WasmError('missing kernel stack depth validation')
                local_stacks.append(row)
            if 'POSTGAMMA_RUNTIME ' in line:
                row = dict(item.split('=', 1) for item in line.split('POSTGAMMA_RUNTIME ', 1)[1].split())
                for key in ('role_process_launches', 'forbidden_process_launch_attempts', 'unsupported_role_requests',
                            'role_threads_active', 'runnable_sessions', 'running_quantums', 'execution_tokens_active'):
                    if row.get(key) != '0': raise WasmError('role accounting failed: ' + key)
                if row.get('provider') != 'pooled': raise WasmError('expected pooled carriers')
                roles.append(row)
        if len(local_stacks) != 3: raise WasmError('missing per-class stack records')
        stacks.extend(local_stacks); healthy.append(module)
    if len(roles) != len(healthy) or len(fatal) != sum(name.startswith('fatal-') for name in SCENARIOS):
        raise WasmError('incomplete healthy/fatal lifecycle evidence')
    # Class peaks need not occur at the same instant; their sum is an upper
    # bound, with the private 4 MiB host driver and 4 MiB control stack added.
    class_peaks = {name: max(int(row['peak_reservation_bytes']) for row in stacks if row['class'] == name)
                   for name in ('dedicated', 'client', 'parallel')}
    io_stack = next(r['stack_bytes'] for r in policy['thread_roles'] if r['role'] == 'io_owner')
    if any(m.get('ioStackBytes') not in (0, io_stack) for m in modules):
        raise WasmError('missing or unexpected I/O stack measurement')
    stack_bound = sum(class_peaks.values()) + 4194304 + policy['control_stack_bytes'] + max(m['ioStackBytes'] for m in modules)
    reserved = sum(r['count'] * r['stack_bytes'] for r in policy['thread_roles']) + policy['control_stack_bytes']
    if stack_bound > reserved or layout['__data_end'] > policy['static_reserve_bytes'] or layout['__tls_size'] > policy['tls_reserve_per_worker_bytes']:
        raise WasmError('measured stack/static/TLS layout exceeds reserves')
    return {'kind': 'postgamma.wasm-sql-resource-budget', 'status': 'passed',
            'validated_scope': {'target': policy['target'], 'mode': mode, 'browser': 'chromium', 'storage': ['ephemeral-wasmfs', 'opfs']},
            'kernel_resource_policy_frozen': policy['status'] == 'validated',
            'healthy_modules': len(healthy), 'fatal_modules': len(fatal),
            'peak_pthreads_sampled': max(m['peakThreads'] for m in modules), 'precreated_pthread_workers': pool,
            'linear_memory_peak_bytes': max(m['peakMemory'] for m in modules),
            'worker_queue_peak_bytes': max(m['peakQueuedBytes'] for m in modules),
            'host_query_queue_peak_bytes': max(m.get('peakHostQueuedBytes', 0) for m in modules),
            'pending_commands_peak': max(m['peakPending'] for m in modules),
            'stack_class_peaks': class_peaks, 'stack_reservation_upper_bound_bytes': stack_bound,
            'layout': layout, 'data_end_includes_control_stack': True,
            'normal_native_resources_released': True, 'fatal_worker_termination_requested': True,
            'synchronous_fatal_vm_reclamation_verified': False,
            'outside_linear_vm_rss_measured': False,
            'sampling_note': 'Lifecycle and command samples; periodic diagnostics are opt-in and can coalesce. Transient peaks may be missed. The fixed SDK pool is the hard thread bound.',
            'sql_limits': policy['sql_limits']}
