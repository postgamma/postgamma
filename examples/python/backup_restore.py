import io

import postgamma


archive = io.BytesIO()
with postgamma.Database("source.pgm") as source:
    with source.connect(autocommit=True) as connection:
        connection.execute("create table notes(id bigint primary key, body text)")
        connection.execute("insert into notes values ($1, $2)", [1, "saved"])
    source.logical_dump(
        archive,
        flags=postgamma.LogicalFlags.NO_OWNER
        | postgamma.LogicalFlags.NO_PRIVILEGES,
    )

archive.seek(0)
with postgamma.Database("restored.pgm") as restored:
    restored.logical_restore(archive)
    with restored.connect(autocommit=True) as connection:
        count = connection.execute("select count(*) from notes").fetchone()[0]
        print(f"Restored rows: {count}")
