import postgamma


with postgamma.connect("vectors.pgm", autocommit=True) as connection:
    bundled = {
        extension.sql_name: extension
        for extension in connection.database.bundled_extensions()
    }
    vector = bundled["vector"]
    print(f"Bundled pgvector {vector.version}")

    connection.execute("create extension if not exists vector")
    connection.execute(
        "create table if not exists documents("
        "id bigint primary key, label text not null, embedding vector(3) not null)"
    )
    connection.execute(
        "insert into documents values "
        "(1, 'alpha', '[1,1,1]'), "
        "(2, 'beta', '[2,2,2]'), "
        "(3, 'gamma', '[3,3,3]') "
        "on conflict (id) do update set "
        "label = excluded.label, embedding = excluded.embedding"
    )
    connection.execute(
        "create index if not exists documents_embedding_hnsw "
        "on documents using hnsw (embedding vector_l2_ops)"
    )
    nearest = connection.execute(
        "select id, label from documents "
        "order by embedding <-> $1::vector limit 1",
        ["[1,1,1]"],
    ).fetchone()
    print(nearest)
