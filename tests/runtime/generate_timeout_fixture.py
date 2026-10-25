"""Exercise PostgreSQL's actual timeout algorithm with a controlled clock."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "buildsys"))
import postgresql_adapter


def without_includes(source: str) -> str:
    return re.sub(r"^#include .*\n", "", source, flags=re.MULTILINE)


adapter = postgresql_adapter.load_adapter(ROOT / "manifests/postgresql/adapter.json")
postgres = Path(sys.argv[2])
path = "src/backend/utils/misc/timeout.c"
edits = [edit for edit in adapter["generated_support"]["edits"] if edit["path"] == path]
source = postgresql_adapter.apply_non_overlapping_edits(
    edits, (postgres / path).read_text()
)
runtime = (ROOT / "runtime/src/postgres_backend_runtime.c").read_text()
header = (postgres / "src/include/utils/timeout.h").read_text()
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
dispatch = runtime.index("void\npostgamma_dispatch_pending_signals(void)")
dispatch_end = runtime.index("\n}\n", dispatch) + 3
output.write_text(without_includes(header) + without_includes(source)
                  + runtime[dispatch:dispatch_end])
