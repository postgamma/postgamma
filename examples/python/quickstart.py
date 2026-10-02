import postgamma


cluster = "agent.pgm"

# The default mode opens an existing cluster or creates a missing one.
with postgamma.connect(cluster, autocommit=True) as connection:
    connection.execute(
        "create table if not exists memory("
        "id bigint primary key, body jsonb not null)"
    )
    connection.execute(
        "insert into memory values ($1, $2) "
        "on conflict (id) do update set body = excluded.body",
        [1, {"kind": "plan", "score": 0.98}],
    )

# OPEN_EXISTING makes a misspelled or missing path fail.
with postgamma.connect(
    cluster,
    mode=postgamma.OpenMode.OPEN_EXISTING,
    autocommit=True,
) as connection:
    row = connection.execute(
        "select id, body from memory where id = $1", [1]
    ).fetchone()
    print(row)

print(f"PostgreSQL cluster directory: {cluster}")
