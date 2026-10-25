"""Compile the real immutable GUC mapping against small catalog fixtures."""

import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / "runtime/src/postgres_guc_runtime.c").read_text()
start = source.index("static pthread_once_t PostgammaGucCatalogOnce")
end = source.index("\nvoid *\npostgamma_guc_slot_address(", start)
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(source[start:end])
