"""Candidate source overlays and weekly suite identities cannot reuse native passes."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'buildsys'))
from wasm_upstream import attach_source, overlay, tracked_inputs
from wasm_toolchain import capture
from run_upgrade_audit import ensure_candidate_worktree
from wasm_ci import stages

class WasmUpstreamTests(unittest.TestCase):
    def test_candidate_overlay_preserves_current_inputs_and_excludes_private_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)/'root';root.mkdir()
            def git(*args):return capture(['git','-C',str(root),'-c','commit.gpgsign=false',*args])
            git('init');git('config','user.name','test');git('config','user.email','test@example.invalid')
            for name in ('changed','deleted','cached-delete'):(root/name).write_text('original')
            git('add','.');git('commit','-m','base')
            commit=git('rev-parse','HEAD')
            (root/'changed').write_text('current')
            (root/'deleted').unlink()
            git('rm','--cached','cached-delete')
            (root/'new').write_text('new code');git('add','new')
            (root/'private-conversation.md').write_text('private')
            inputs=tracked_inputs(root)
            project=Path(temporary)/'candidate'
            ensure_candidate_worktree(root,commit,project)
            overlay(root,project,inputs)
            self.assertEqual((project/'changed').read_text(),'current')
            self.assertEqual((project/'new').read_text(),'new code')
            self.assertFalse((project/'deleted').exists())
            self.assertFalse((project/'cached-delete').exists())
            self.assertFalse((project/'private-conversation.md').exists())
            self.assertEqual(git('rev-parse','HEAD'),commit)
            self.assertEqual((root/'cached-delete').read_text(),'original')
            git('worktree','remove','--force',str(project))

    def test_candidate_index_matches_selected_source_without_changing_main_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            def git(root, *args):
                return capture(['git', '-C', str(root), '-c', 'commit.gpgsign=false', *args])
            def repository(name):
                root = base / name; root.mkdir()
                git(root, 'init'); git(root, 'config', 'user.name', 'test')
                git(root, 'config', 'user.email', 'test@example.invalid')
                return root
            source = repository('source')
            (source / 'version').write_text('old')
            git(source, 'add', '.'); git(source, 'commit', '-m', 'old')
            old = git(source, 'rev-parse', 'HEAD')
            (source / 'version').write_text('new')
            git(source, 'commit', '-am', 'new')
            new = git(source, 'rev-parse', 'HEAD')
            root = repository('root')
            (root / '.gitmodules').write_text('[submodule "postgres"]\n path = postgres\n url = ' + str(source) + '\n')
            git(root, 'config', 'submodule.postgres.url', str(source))
            git(root, 'add', '.gitmodules')
            git(root, 'update-index', '--add', '--cacheinfo', '160000,' + old + ',postgres')
            git(root, 'commit', '-m', 'pin')
            project = base / 'candidate'
            ensure_candidate_worktree(root, git(root, 'rev-parse', 'HEAD'), project)
            attach_source(project, 'postgres', source, new)
            self.assertTrue(git(project, 'submodule', 'status').startswith(new + ' '))
            self.assertIn(old, git(root, 'ls-files', '--stage', 'postgres'))
            self.assertIn(new, git(project, 'ls-files', '--stage', 'postgres'))
            git(source, 'worktree', 'remove', str(project / 'postgres'))
            git(root, 'worktree', 'remove', '--force', str(project))

    def test_weekly_keeps_real_sql_storage_vector_package_and_matrix_gates(self):
        names=[name for name,_ in stages('weekly')]
        for name in ('sql','storage','vector','package','matrix'):self.assertIn(name,names)
        self.assertLess(names.index('sql'),names.index('vector'))
        self.assertLess(names.index('vector'),names.index('storage'))
        self.assertLess(names.index('package'),names.index('matrix'))

    def test_mirror_download_keeps_canonical_source_identity(self):
        import json
        from unittest.mock import patch
        from wasm_toolchain import Configuration, write_json
        from wasm_upstream import candidate
        with tempfile.TemporaryDirectory() as temporary:
            base=Path(temporary)
            def git(root,*args):
                return capture(['git','-C',str(root),'-c','commit.gpgsign=false',*args])
            def repository(name):
                path=base/name;path.mkdir()
                git(path,'init');git(path,'config','user.name','test');git(path,'config','user.email','test@example.invalid')
                return path
            source=repository('source')
            (source/'configure.ac').write_text('AC_INIT([PostgreSQL], [20devel])\n')
            git(source,'add','.');git(source,'commit','-m','source')
            revision=git(source,'rev-parse','HEAD')
            root=repository('project')
            canonical='https://git.postgresql.org/git/postgresql.git'
            (root/'.gitmodules').write_text('[submodule "postgres"]\n path = postgres\n url = '+canonical+'\n[submodule "third_party/pgvector"]\n path = third_party/pgvector\n url = vector\n')
            write_json(root/'manifests/upstream.json',{'repository':canonical,'commit':revision})
            write_json(root/'profile.json',{'target':'wasm32-emscripten-pthreads'})
            (root/'wasm').mkdir();(root/'wasm/placeholder').write_text('package')
            git(root,'add','.')
            attach_source(root,'postgres',source,revision)
            attach_source(root,'third_party/pgvector',source,revision)
            git(root,'commit','-m','pinned')
            before=tracked_inputs(root)
            config=Configuration(root,root/'build',root/'.deps',root/'.deps/sdk',root/'profile.json',
                {'target':'wasm32-emscripten-pthreads'},'debug',False,jobs=2)
            def suite(selected,*args,**kwargs):
                self.assertTrue(kwargs['candidate'])
                self.assertEqual(json.loads((selected.root/'manifests/upstream.json').read_text())['repository'],canonical)
                receipt=selected.build/'ci.json';write_json(receipt,{'status':'passed'});return receipt
            mirror='https://github.com/postgres/postgres.git'
            with patch('wasm_upstream.source_inputs',return_value={'postgres_url':mirror,'postgres_commit':revision}), \
                 patch('wasm_upstream.run') as fetch, patch('wasm_upstream.run_suite',side_effect=suite):
                report=json.loads(candidate(config,'','master','weekly').read_text())
                self.assertIn(mirror,fetch.call_args.args[0])
            self.assertEqual(report['postgres_repository'],mirror)
            self.assertEqual(report['status'],'passed')
            self.assertEqual(tracked_inputs(root),before)

if __name__=='__main__':unittest.main()
