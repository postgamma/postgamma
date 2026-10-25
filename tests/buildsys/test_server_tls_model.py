"""Executable TLS optimization must retain the default DSO loading contract."""

from __future__ import annotations

import os
import itertools
import platform
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "x86_64",
                     "ELF x86-64 relocation checks")
class ServerTLSModelTests(unittest.TestCase):
    def test_executable_exports_tls_to_a_late_loaded_extension(self) -> None:
        compilers = [shlex.split(os.environ.get("CC", "cc"))]
        clang = shutil.which("clang") or shutil.which("clang-22")
        if clang:
            compilers.append([clang])
        for compiler, direct in itertools.product(compilers, (False, True)):
            with self.subTest(compiler=compiler, direct=direct), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                (work / "postgamma").mkdir()
                (work / "postgamma/backend_state_layout.h").write_text(
                    "typedef enum { TEST_SLOT, POSTGAMMA_BACKEND_STATE_SLOT_COUNT } "
                    "PostgammaBackendStateSlot;\n"
                )
                (work / "postgamma/backend_state_direct.h").write_text(
                    '#include <stdint.h>\n'
                    'static inline size_t postgamma_backend_state_prepared_offset('
                    'PostgammaBackendStateSlot slot) { return slot == TEST_SLOT ? 0 : SIZE_MAX; }\n'
                )
                (work / "extension.c").write_text(
                    '#include "postgamma/backend_state_runtime.h"\n'
                    'void *extension_address(void) {\n'
                    '    static int template_value;\n'
                    '    return ' + ('postgamma_backend_state_direct_address(' if direct else
                                      'postgamma_backend_state_cached_address(') +
                    'TEST_SLOT, &template_value);\n}\n'
                )
                (work / "host.c").write_text(
                    '#include "postgamma/backend_state_runtime.h"\n'
                    '#include "check.h"\n#include <dlfcn.h>\n#include <pthread.h>\n'
                    'POSTGAMMA_THREAD_LOCAL void *const *'
                    'PostgammaCurrentBackendStateAddresses;\n'
                    'POSTGAMMA_THREAD_LOCAL unsigned char *PostgammaCurrentPreparedRoleState;\n'
                    'void postgamma_runtime_contract_violation(void) { abort(); }\n'
                    'static pthread_barrier_t barrier;\n'
                    'static void *(*extension_address)(void);\n'
                    'static void *worker(void *ignored) {\n'
                    '    int value = 7; void *slots[] = { &value };\n'
                    '    (void) ignored;\n'
                    '    PostgammaCurrentBackendStateAddresses = slots;\n'
                    '    PostgammaCurrentPreparedRoleState = (unsigned char *) &value;\n'
                    '    pthread_barrier_wait(&barrier);\n'
                    '    pthread_barrier_wait(&barrier);\n'
                    '    CHECK(extension_address() == &value);\n'
                    '    return NULL;\n}\n'
                    'int main(int argc, char **argv) {\n'
                    '    int value = 3; void *slots[] = { &value };\n'
                    '    pthread_t thread; void *module;\n'
                    '    CHECK(argc == 2);\n'
                    '    PostgammaCurrentBackendStateAddresses = slots;\n'
                    '    PostgammaCurrentPreparedRoleState = (unsigned char *) &value;\n'
                    '    CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);\n'
                    '    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);\n'
                    '    pthread_barrier_wait(&barrier);\n'
                    '    module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);\n'
                    '    CHECK(module != NULL);\n'
                    '    extension_address = dlsym(module, "extension_address");\n'
                    '    CHECK(extension_address != NULL);\n'
                    '    CHECK(extension_address() == &value);\n'
                    '    pthread_barrier_wait(&barrier);\n'
                    '    CHECK(pthread_join(thread, NULL) == 0);\n'
                    '    CHECK(dlclose(module) == 0);\n'
                    '    CHECK(pthread_barrier_destroy(&barrier) == 0);\n'
                    '    return 0;\n}\n'
                )
                flags = ["-std=c11", "-D_GNU_SOURCE", "-O2", "-DNDEBUG",
                         "-Wall", "-Wextra", "-Werror", f"-I{work}",
                         f"-I{ROOT / 'runtime/include'}", f"-I{ROOT / 'tests'}"]
                if direct:
                    flags += ["-DPOSTGAMMA_PREPARED_ROLE_STATE", "-DPOSTGAMMA_DIRECT_ROLE_STATE"]

                def run(arguments: list[str]) -> str:
                    result = subprocess.run(arguments, cwd=work, capture_output=True,
                                            text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    return result.stdout

                run([*compiler, *flags, "-fPIC", "-shared", "extension.c",
                     "-o", "extension.so"])
                for enabled in (False, True):
                    define = ["-DPOSTGAMMA_SERVER_LOCAL_EXEC_TLS"] if enabled else []
                    run([*compiler, *flags, *define, "-c", "host.c", "-o", "host.o"])
                    run([*compiler, "host.o", "-Wl,--export-dynamic", "-pthread",
                         "-ldl", "-o", "host"])
                    run([str(work / "host"), str(work / "extension.so")])
                    symbols = run(["nm", "-D", "host"])
                    self.assertIn("PostgammaCurrentBackendStateAddresses", symbols)
                    if enabled:
                        relocations = run(["readelf", "-rW", "host.o"])
                        self.assertRegex(relocations, "R_X86_64_TPOFF32.*"
                                         "PostgammaCurrentBackendStateAddresses")
                        if direct:
                            self.assertRegex(relocations, "R_X86_64_TPOFF32.*"
                                             "PostgammaCurrentPreparedRoleState")
                # The extension must still use the dynamic TLS model, even
                # when its host opted into local-exec for core objects.
                relocations = run(["readelf", "-rW", "extension.so"])
                self.assertRegex(relocations, "R_X86_64_DTPMOD64.*"
                                 + ("PostgammaCurrentPreparedRoleState" if direct else
                                    "PostgammaCurrentBackendStateAddresses"))


if __name__ == "__main__":
    unittest.main()
