# Vector search with pgvector

PostGamma 0.1.0a1 bundles pgvector 0.8.6 into the embedded kernel. The wheel
and static C SDK contain its native code, control file, and installation SQL;
no PostgreSQL server package or separately loaded `vector.so` is required.

## Check availability

Bundled means that the code is available to a cluster. It does not silently
install SQL objects into every logical database. Inspect the build and then
enable the extension once in each database that needs it:

```python
import postgamma

database = postgamma.Database("vectors.pgm")
available = {item.sql_name: item for item in database.bundled_extensions()}
print(available["vector"].version)  # 0.8.6

with database.connect(autocommit=True) as connection:
    connection.execute("create extension if not exists vector")
```

`CREATE EXTENSION` is transactional PostgreSQL DDL. After it commits, the
extension remains installed when the embedded instance closes and reopens.
An unbundled native extension still fails closed instead of loading a host
shared object.

## Run a nearest-neighbor query

The complete runnable example creates a persistent cluster, enables pgvector,
builds an HNSW index, and performs a parameterized search:

```python
--8<-- "examples/python/vector_search.py"
```

Run it with:

```bash
python vector_search.py
```

It prints:

```text
Bundled pgvector 0.8.6
(1, 'alpha')
```

Python strings are suitable parameters when SQL gives PostgreSQL the target
type, as in `$1::vector`. Vector result OIDs are extension-defined, so cast a
vector to `text` for a string result or register a custom PostGamma codec when
the application needs a dedicated Python vector class.

## Choose an index

HNSW supports strong query performance without a training step and can be
created on an empty table:

```sql
CREATE INDEX documents_embedding_hnsw
ON documents USING hnsw (embedding vector_l2_ops);
```

IVFFlat should normally be built after representative data has been loaded:

```sql
CREATE INDEX documents_embedding_ivfflat
ON documents USING ivfflat (embedding vector_l2_ops)
WITH (lists = 100);
ANALYZE documents;
```

The distance operator in the query must match the operator class selected by
the index. The common pairs are:

| Search | Operator | Operator class |
| --- | --- | --- |
| Euclidean distance | `<->` | `vector_l2_ops` |
| Negative inner product | `<#>` | `vector_ip_ops` |
| Cosine distance | `<=>` | `vector_cosine_ops` |
| L1 distance | `<+>` | `vector_l1_ops` |

Refer to the pgvector 0.8.6 SQL behavior for additional `halfvec`, `bit`, and
`sparsevec` types and their compatible operator classes.

## Sessions, workers, and settings

`hnsw.ef_search`, `hnsw.iterative_scan`, `ivfflat.probes`, and
`ivfflat.iterative_scan` are PostgreSQL settings. A `SET` applies to that
logical connection; it does not leak to another connection or another live
embedded instance. `SET LOCAL` remains scoped to the current transaction.

PostGamma's release gate builds HNSW and IVFFlat indexes, resolves pgvector's
parallel-build worker entries through the static extension registry, executes
vector functions in PostgreSQL parallel workers, runs searches concurrently in
two live clusters, and verifies the indexes after close and reopen.

## Static C SDK

The C API does not add a separate vector-specific surface. Open an instance and
connection with the normal `pgm_*` API, execute `CREATE EXTENSION vector`, and
send the same SQL shown above. This keeps pgvector behavior, types, errors, and
query planning identical across the Python and C forms of the product.

The static SDK archive includes `licenses/LICENSE.pgvector`; applications that
redistribute the linked extension must preserve the supplied notices.
