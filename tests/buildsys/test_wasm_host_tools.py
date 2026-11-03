"""Check old-host tool selection, compiler Node selection, and archive integrity."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
import wasm_host_tools as host
import wasm_toolchain as tools


class HostToolsTests(unittest.TestCase):
    def config(self, root):
        profile = ROOT / "manifests/profiles/wasm-emscripten.json"
        return tools.Configuration(root, root / "build", root / "deps", root / "sdk",
                                   profile, tools.read_json(profile), "release", False)

    def test_host_abi_selects_only_tools_that_need_replacement(self):
        config = self.config(ROOT)
        with mock.patch.object(host.platform, "system", return_value="Linux"), \
             mock.patch.object(host.platform, "machine", return_value="x86_64"):
            for version, expected in [("2.17", {"node", "binaryen", "llvm"}), ("2.23", {"node"}),
                                      ("2.28", set()), ("2.39", set())]:
                with self.subTest(version=version), \
                     mock.patch.object(host.platform, "libc_ver", return_value=("glibc", version)):
                    self.assertEqual(set(host.tool_paths(config)), expected)

    def test_explicit_tools_are_preserved(self):
        config = self.config(ROOT)
        with mock.patch.object(host.platform, "system", return_value="Linux"), \
             mock.patch.object(host.platform, "machine", return_value="x86_64"), \
             mock.patch.object(host.platform, "libc_ver", return_value=("glibc", "2.17")):
            config.node_override = "/custom/node"
            self.assertEqual(set(host.tool_paths(config)), {"binaryen", "llvm"})
            config.external_sdk = True
            self.assertEqual(host.tool_paths(config), {})

    def test_node_override_reaches_emscripten_and_not_only_browser_drivers(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            config.external_sdk = True
            config.node_override = str(config.root / "node22")
            emscripten = config.sdk / "upstream/emscripten"
            emscripten.mkdir(parents=True)
            for file in [emscripten / "emcc.py", config.sdk / ".emscripten", Path(config.node_override),
                         config.sdk / "clang", config.sdk / "wasm-ld", config.sdk / "bin/wasm-opt"]:
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text("test input")
            def capture(command, env=None, cwd=None):
                self.assertEqual(env["EM_NODE_JS"], config.node_override)
                if "em-config.py" in command[1]:
                    key = command[-1]
                    if key == "NODE_JS":
                        return repr([env["EM_NODE_JS"]])
                    return str(config.sdk if key.endswith("ROOT") else config.deps / key)
                if command[0] == config.node_override:
                    return "v22.23.4"
                if command[1].endswith("emcc.py"):
                    return "emcc (Emscripten gcc/clang-like replacement) 6.0.9"
                return "test version"
            with mock.patch.object(tools, "capture", side_effect=capture), \
                 mock.patch.object(tools.shutil, "which", return_value="/node22/npm"), \
                 mock.patch.dict(tools.os.environ, {"EM_NODE_JS": "/unrelated/node24"}):
                identity, env = tools.sdk_tools(config)
            self.assertEqual(identity["node"], config.node_override)
            self.assertEqual(identity["node_version"], "v22.23.4")
            self.assertEqual(env["EM_NODE_JS"], config.node_override)

    def test_linker_overlay_preserves_arguments_sdk_bytes_and_process_environment(self):
        with tempfile.TemporaryDirectory(prefix="wasm tools ' ") as temporary:
            config = self.config(Path(temporary))
            original = config.sdk / "upstream/bin"
            original.mkdir(parents=True)
            (original / "lld").write_text("original SDK linker")
            (original / "wasm-ld").symlink_to("lld")
            (original / "clang").write_text("original SDK compiler")
            before = {p.name: tools.digest(p) for p in original.iterdir()}
            llvm = config.deps / "host-tools/llvm-glibc-2.28/bin"
            pin = config.profile["legacy_linux_tools"]["linker_runtime"]
            payload_root = config.root / "payload"
            lib = payload_root / pin["library_directory"]
            lib.mkdir(parents=True)
            loader = lib / "ld-linux-x86-64.so.2"
            loader.write_text("#!" + sys.executable + "\nimport json,os,sys\n"
                              "print(json.dumps([sys.argv[1:],os.environ.get('LD_LIBRARY_PATH')]))\n")
            loader.chmod(0o755)
            (lib / "libc.so.6").write_text("private libc")
            payload = config.root / "pkg-runtime.tar.zst"
            # Mock only decompression; exercise the real wrapper, receipts and SDK symlinks.
            with tarfile.open(payload, "w") as archive:
                archive.add(payload_root / "x86_64-conda-linux-gnu", "x86_64-conda-linux-gnu")
            archive = config.root / pin["filename"]
            with zipfile.ZipFile(archive, "w") as package:
                package.write(payload, payload.name)
            pin["sha256"] = tools.digest(archive)
            pin["url"] = archive.as_uri()

            def extract(command, log, cwd):
                with tarfile.open(command[-1]) as package:
                    package.extractall(cwd, filter="data")

            with mock.patch.object(host, "tool_paths", return_value={"llvm": llvm}), \
                 mock.patch.object(host, "cmake_tool", return_value=Path("cmake")), \
                 mock.patch.object(host, "run", side_effect=extract) as unpack:
                host.setup_linker(config, config.root / "commands.log")
                state = host.linker_identity(config)
                self.assertEqual(state["sdk_wasm_ld_sha256"], before["wasm-ld"])
                self.assertEqual((llvm / "clang").resolve(), original / "clang")
                arguments = ["file with spaces.o", "$(false)", "--", ""]
                env = dict(os.environ, LD_LIBRARY_PATH="/caller/libraries")
                result = subprocess.run([str(llvm / "wasm-ld"), *arguments], env=env,
                                        text=True, capture_output=True, check=True)
                argv, library_path = json.loads(result.stdout)
                self.assertEqual(argv[:3], ["--library-path", str(llvm.parent / "runtime" / pin["library_directory"]),
                                           str(original / "wasm-ld")])
                self.assertEqual(argv[3:], arguments)
                self.assertEqual(library_path, env["LD_LIBRARY_PATH"])
                self.assertEqual({p.name: tools.digest(p) for p in original.iterdir()}, before)
                host.setup_linker(config, config.root / "commands.log")
                self.assertEqual(unpack.call_count, 1)
                runtime_libc = llvm.parent / "runtime" / pin["library_directory"] / "libc.so.6"
                runtime_libc.write_text("corrupt libc")
                with self.assertRaisesRegex(tools.WasmError, "identity mismatch"):
                    host.linker_identity(config)
                host.setup_linker(config, config.root / "commands.log")
                self.assertEqual(unpack.call_count, 2)
                self.assertEqual(runtime_libc.read_text(), "private libc")
                (llvm / "clang").unlink()
                (llvm / "clang").symlink_to(original / "lld")
                with self.assertRaisesRegex(tools.WasmError, "overlay mismatch"):
                    host.linker_identity(config)

    def test_missing_linker_runtime_fails_read_only_without_installation(self):
        with tempfile.TemporaryDirectory() as temporary:
            config = self.config(Path(temporary))
            llvm = config.deps / "host-tools/llvm-glibc-2.28/bin"
            with mock.patch.object(host, "tool_paths", return_value={"llvm": llvm}), \
                 mock.patch.object(host, "download") as download:
                with self.assertRaisesRegex(tools.WasmError, "runtime is missing"):
                    host.linker_identity(config)
                download.assert_not_called()
                self.assertFalse(config.deps.exists())

    def test_cached_archive_is_verified_and_corruption_cannot_be_extracted(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            archive = cache / "tools.tar.gz"
            archive.write_bytes(b"verified bytes")
            pin = {"filename": archive.name, "url": "file://" + str(archive),
                   "sha256": tools.digest(archive)}
            with mock.patch.object(host.urllib.request, "urlopen") as fetch:
                self.assertEqual(host.download(pin, cache), archive)
                fetch.assert_not_called()
            archive.write_bytes(b"corrupt bytes")
            with self.assertRaisesRegex(tools.WasmError, "checksum mismatch"):
                host.download(pin, cache)
            self.assertFalse((cache / (archive.name + ".partial")).exists())


if __name__ == "__main__":
    unittest.main()
