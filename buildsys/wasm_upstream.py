#!/usr/bin/env python3
"""Exercise an exact PostgreSQL revision in an isolated project worktree."""
from __future__ import annotations
import argparse
from dataclasses import replace
import os
from pathlib import Path
import re
import shutil
import time
from plan_upstream_ci import source_inputs
from run_upgrade_audit import ensure_candidate_worktree
from wasm_ci import run_suite
from wasm_toolchain import (WasmError, capture, configuration, digest, fingerprint,
                            read_json, run, run_id, write_json)


def tracked_inputs(root):
    names = capture(['git','-C',str(root),'ls-files','-z']).split('\0')
    result = {}
    for name in names:
        if not name: continue
        path = root/name
        if path.is_symlink(): result[name] = {'symlink':os.readlink(path)}
        elif path.is_file(): result[name] = {'sha256':digest(path)}
        elif not path.exists(): result[name] = {'deleted':True}
    return result


def overlay(root, project, inputs):
    for name in capture(['git','-C',str(project),'ls-files','-z']).split('\0'):
        target = project/name
        if name and name not in inputs and (target.is_symlink() or target.is_file()): target.unlink()
    for name, value in inputs.items():
        source, target = root/name, project/name
        if target.is_symlink() or target.is_file(): target.unlink()
        if value.get('deleted'): continue
        target.parent.mkdir(parents=True,exist_ok=True)
        if 'symlink' in value: target.symlink_to(value['symlink'])
        else: shutil.copy2(source,target)


def attach_source(project, name, repository, revision):
    destination = project / name
    if destination.is_dir() and not any(destination.iterdir()):
        destination.rmdir()
    ensure_candidate_worktree(repository, revision, destination)
    # The disposable project's index must describe its selected source revision.
    # Its linked worktree has an independent index; the main source pin is untouched.
    capture(['git', '-C', str(project), 'update-index', '--add', '--cacheinfo',
             '160000,' + revision + ',' + name])


def candidate(config, ref, branch, profile, longjmp='emscripten'):
    current = config.build/'wasm/upstream'/run_id(); current.mkdir(parents=True)
    inputs = tracked_inputs(config.root)
    report = {'kind':'postgamma.wasm-upstream-canary','status':'failed','profile':profile,
              'source_inputs':inputs,'source_identity':fingerprint(inputs),
              'postgamma_commit':capture(['git','-C',str(config.root),'rev-parse','HEAD']),
              'pinned_postgres_commit':capture(['git','-C',str(config.root/'postgres'),'rev-parse','HEAD']),
              'pgvector_commit':capture(['git','-C',str(config.root/'third_party/pgvector'),'rev-parse','HEAD']),
              'stage':'resolve','startedFreeBytes':shutil.disk_usage(config.build).free}
    started = time.monotonic()
    target = None
    try:
        selected = source_inputs(config.root,profile,'' if ref else branch)
        if ref:
            try: commit = capture(['git','-C',str(config.root/'postgres'),'rev-parse','--verify',ref+'^{commit}'])
            except WasmError:
                run(['git','-C',str(config.root/'postgres'),'fetch','--no-tags',selected['postgres_url'],ref],
                    current/'fetch.log',cwd=config.root,timeout=300)
                commit = capture(['git','-C',str(config.root/'postgres'),'rev-parse','FETCH_HEAD^{commit}'])
        else:
            commit = selected['postgres_commit']
            run(['git','-C',str(config.root/'postgres'),'fetch','--no-tags',selected['postgres_url'],commit],
                current/'fetch.log',cwd=config.root,timeout=300)
        if not re.fullmatch('[0-9a-f]{40}|[0-9a-f]{64}',commit): raise WasmError('candidate did not resolve to one commit')
        report.update(requested_ref=ref,postgres_commit=commit,postgres_repository=selected['postgres_url'],branch=branch,stage='worktrees')
        write_json(current/'report.json',report)
        project = current/'project'
        ensure_candidate_worktree(config.root,report['postgamma_commit'],project)
        overlay(config.root,project,inputs)
        for name, repository, revision in (
                ('postgres',config.root/'postgres',commit),
                ('third_party/pgvector',config.root/'third_party/pgvector',
                 report['pgvector_commit'])):
            attach_source(project, name, repository, revision)
        manifest = read_json(project/'manifests/upstream.json')
        manifest.update(commit=commit,ref_name=branch,repository=selected['postgres_url'])
        write_json(project/'manifests/upstream.json',manifest)
        modules = project/'wasm/node_modules'
        modules.symlink_to(config.package/'node_modules',target_is_directory=True)
        report['project']=str(project)
        report['source_version']=re.search(r'AC_INIT\(\[PostgreSQL\], \[([^]]+)\]',(project/'postgres/configure.ac').read_text())[1]
        report['stage']='wasm-validation'
        write_json(current/'report.json',report)
        target = replace(config,root=project,build=project/'build',
                         profile_path=project/config.profile_path.relative_to(config.root),external_sdk=True)
        receipt = run_suite(target,'weekly' if profile=='weekly' else 'basic',longjmp,candidate=True)
        report['suite_report']=str(receipt)
        if tracked_inputs(config.root)!=inputs:
            raise WasmError('tracked project inputs changed during the canary')
        if capture(['git','-C',str(config.root/'postgres'),'rev-parse','HEAD']) != report['pinned_postgres_commit']:
            raise WasmError('pinned source checkout changed during the canary')
        if capture(['git','-C',str(config.root/'third_party/pgvector'),'rev-parse','HEAD']) != report['pgvector_commit']:
            raise WasmError('pgvector source checkout changed during the canary')
        report['status']='passed'
    except Exception as error:
        report['error']=str(error)
        if target is not None:
            pointer = target.build/'wasm/reports/ci.json'
            if pointer.is_file():
                report['suite_report']=read_json(pointer)['run_report']
                suite=read_json(Path(report['suite_report']))
                report['failed_stage']=suite['stages'][-1]['name'] if suite['stages'] else 'preflight'
        report['failure_class']='candidate-preparation' if report['stage']!='wasm-validation' else 'wasm-integration-or-environment'
        raise
    finally:
        report['elapsedSeconds']=time.monotonic()-started
        report['pinned_postgres_after']=capture(['git','-C',str(config.root/'postgres'),'rev-parse','HEAD'])
        report['finishedFreeBytes']=shutil.disk_usage(config.build).free
        write_json(current/'report.json',report)
        write_json(config.build/'wasm/reports/upstream.json',{'status':report['status'],'run_report':str(current/'report.json')})
        print(f"wasm upstream canary: {report['status']}; {current/'report.json'}",flush=True)
    return current/'report.json'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    for name in ('build-dir','deps-dir','sdk-dir','profile'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--target',default='wasm32-emscripten-pthreads')
    parser.add_argument('--mode',default='debug',choices=('debug','release'))
    parser.add_argument('--node',default='');parser.add_argument('--browser-executable',default='')
    parser.add_argument('--longjmp',default='emscripten');parser.add_argument('--jobs',type=int,default=2)
    parser.add_argument('--ref',default='');parser.add_argument('--branch',default='master')
    parser.add_argument('--canary-profile',choices=('daily','weekly'),default='weekly')
    args=parser.parse_args()
    try: candidate(configuration(args),args.ref,args.branch,args.canary_profile,args.longjmp)
    except Exception as error:parser.exit(2,'wasm upstream canary: '+str(error).splitlines()[0]+'\n')

if __name__=='__main__':main()
