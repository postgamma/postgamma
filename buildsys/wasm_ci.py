#!/usr/bin/env python3
"""Run actual browser kernel gates and retain stage timings and failure evidence."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import time
from wasm_toolchain import WasmError, configuration, capture, digest, read_json, run, run_id, write_json


def stages(profile, platform_checks=False):
    result = [('tools', ['doctor-ast', 'doctor-wasm', 'wasm-generator-tools-check']), ('unit', []), ('generation', ['wasm-kernel-generate']),
              ('runtime', ['wasm-runtime-check'])]
    if profile == 'weekly' or platform_checks:
        result += [('platform', ['wasm-platform-check']), ('wait', ['wasm-wait-check']),
                   ('recovery', ['wasm-recovery-check'])]
    result += [('oracle-build', ['reference-build']), ('oracle-install', []),
               ('sql', ['wasm-sql-check'])]
    if profile == 'weekly':
        result += [('vector', ['wasm-vector-check'])]
        result += [('contrib', ['wasm-contrib-check'])]
        result += [('parallel', ['wasm-parallel-check'])]
        result += [('sdk', ['wasm-sdk-check'])]
    result += [('pglite', ['wasm-pglite-check'])]
    if profile == 'weekly':
        result += [('storage', ['wasm-storage-check'])]
        result += [('multitab', ['wasm-multitab-check'])]
        result += [('archive', ['wasm-archive-check'])]
        result += [('logical', ['wasm-logical-check'])]
        result += [('lifecycle', ['wasm-lifecycle-check'])]
        result += [('soak-smoke', ['wasm-soak-smoke'])]
    result += [('package', ['wasm-package-check'])]
    if profile == 'weekly': result += [('bundlers', ['wasm-bundler-check']), ('matrix', ['wasm-matrix-check'])]
    return result


def run_suite(config, profile, longjmp, candidate=False, platform_checks=False, keep_going=False):
    current = config.build/'wasm/ci'/run_id(); current.mkdir(parents=True)
    report = {'kind':'postgamma.wasm-ci','status':'failed','profile':profile,
              'target':config.target,'mode':config.mode,'candidate':candidate,'stages':[],
              'keep_going':keep_going,
              'platform_checks':profile == 'weekly' or platform_checks,
              'source_commit':capture(['git','-C',str(config.root),'rev-parse','HEAD']),
              'driver_sha256':digest(config.root/'buildsys/wasm_ci.py'),
              'scope':'Wasm evidence is independent of native product acceptance.'}
    env = dict(os.environ)
    for name in ('MAKEFLAGS','MFLAGS','MAKELEVEL'): env.pop(name,None)
    assignments = [f'BUILD_DIR={config.build}',f'WASM_DEPS_DIR={config.deps}',
        f'REFERENCE_BUILD={config.build}/wasm/oracle/reference',
        f'WASM_PROFILE={config.profile_path}',
        f'WASM_TARGET={config.target}',f'WASM_MODE={config.mode}',f'JOBS={config.jobs}',
        f'WASM_LONGJMP={longjmp}', 'WASM_SQL_SCENARIO=all', 'WASM_STORAGE_SCENARIO=storage-all',
        'WASM_VECTOR_SCENARIO=vector-all','WASM_EXTENSIONS=none','WASM_PACKAGE_EXTENSIONS=standard']
    assignments += ['WASM_PGLITE_ENGINES=' + ('chromium,firefox,webkit' if profile == 'weekly' else 'chromium'),
                    'WASM_PGLITE_STORAGE=' + ('ephemeral,opfs' if profile == 'weekly' else 'ephemeral')]
    if candidate: assignments.append('POSTGRES_CANDIDATE_PROBE=1')
    if config.external_sdk: assignments.append('WASM_EMSDK_DIR='+str(config.sdk))
    if config.node_override: assignments.append('WASM_NODE='+config.node_override)
    if config.browser_override: assignments.append('WASM_BROWSER_EXECUTABLE='+config.browser_override)
    started = time.monotonic()
    dependencies = {'generation': ('tools',), 'oracle-build': ('tools',),
                    'oracle-install': ('oracle-build',), 'sql': ('generation', 'oracle-install'),
                    'vector': ('generation', 'oracle-install'), 'logical': ('generation', 'oracle-install'),
                    'matrix': ('package',), 'bundlers': ('package',)}
    for name in ('runtime', 'pglite', 'storage', 'multitab', 'archive', 'contrib', 'parallel', 'sdk', 'lifecycle', 'soak-smoke', 'package'): dependencies[name] = ('generation',)
    for name in ('platform', 'wait', 'recovery'): dependencies[name] = ('tools',)
    try:
        for name, targets in stages(profile, platform_checks):
            row = {'name':name,'status':'failed','freeBytesBefore':shutil.disk_usage(config.build).free}
            prior = {entry['name']:entry['status'] for entry in report['stages']}
            unavailable = [dep for dep in dependencies.get(name, ()) if prior.get(dep) != 'passed']
            if unavailable:
                row.update(status='not-run', reason='prerequisite did not pass: ' + ', '.join(unavailable))
                report['stages'].append(row)
                write_json(current/'report.json', report)
                print('wasm CI: ' + name + ' not-run; ' + row['reason'], flush=True)
                continue
            report['stages'].append(row); begin = time.monotonic()
            command = ['make','-C',str(config.root),*targets,*assignments]
            if name == 'oracle-install':
                # temp-install also initializes an upstream regression template,
                # which rejects root. Our SQL oracle owns its separate cluster
                # and drops privileges; only a private installation is needed.
                reference = config.build / 'wasm/oracle/reference'
                command = ['make', '-C', str(reference), 'install',
                           'DESTDIR=' + str(reference / 'tmp_install'), f'-j{config.jobs}']
            elif name == 'unit':
                import sys
                command = [sys.executable, '-m', 'unittest', 'discover', '-s', str(config.root/'tests/buildsys'), '-p', 'test_wasm*.py']
            row['command']=command
            print('wasm CI: '+name,flush=True)
            try:
                run(command,current/(name+'.log'),env,config.root,timeout=7200)
                row['status']='passed'
            except Exception as error:
                row['error']=str(error)
                if not keep_going: raise
            finally:
                row['elapsedSeconds']=time.monotonic()-begin
                row['freeBytesAfter']=shutil.disk_usage(config.build).free
                write_json(current/'report.json',report)
        if profile == 'weekly' and any(row['name'] == 'matrix' and row['status'] == 'passed' for row in report['stages']):
            pointer = read_json(config.build/'wasm/reports/matrix.json')
            report['matrix_report'] = pointer['run_report']
            report['browser_failures'] = read_json(Path(pointer['run_report']))['observedFailures']
        failures = [row['name'] for row in report['stages'] if row['status'] != 'passed']
        if failures:
            raise WasmError('stages failed or not run: ' + ', '.join(failures))
        report['status']='passed'
    except Exception as error:
        report['error']=str(error)
        raise
    finally:
        report['not_run'] = [name for name, _ in stages(profile, platform_checks)
                             if name not in {row['name'] for row in report['stages']}]
        report['not_run'] += [row['name'] for row in report['stages'] if row['status'] == 'not-run']
        report['elapsedSeconds']=time.monotonic()-started
        write_json(current/'report.json',report)
        write_json(config.build/'wasm/reports/ci.json',{'status':report['status'],'run_report':str(current/'report.json')})
        print(f"wasm CI: {report['status']}; {current/'report.json'}",flush=True)
    return current/'report.json'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    for name in ('build-dir','deps-dir','sdk-dir','profile'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--target',default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode',default='debug',choices=('debug','release'))
    parser.add_argument('--node',default='');parser.add_argument('--browser-executable',default='')
    parser.add_argument('--longjmp',default='emscripten');parser.add_argument('--jobs',type=int,default=2)
    parser.add_argument('--suite',choices=('basic','weekly'),default='basic')
    parser.add_argument('--candidate-probe',action='store_true')
    parser.add_argument('--platform-checks',action='store_true')
    parser.add_argument('--keep-going',action='store_true',help='retain failures and run stages whose prerequisites passed')
    args=parser.parse_args()
    try: run_suite(configuration(args),args.suite,args.longjmp,candidate=args.candidate_probe,
                   platform_checks=args.platform_checks,keep_going=args.keep_going)
    except Exception as error: parser.exit(2,'wasm CI: '+str(error).splitlines()[0]+'\n')

if __name__=='__main__':main()
