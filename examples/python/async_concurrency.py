import asyncio

import postgamma


async def main() -> None:
    async with postgamma.AsyncDatabase("agent.pgm", worker_count=4) as database:
        first = await database.connect(autocommit=True)
        second = await database.connect(autocommit=True)
        try:
            await first.execute(
                "create table if not exists memory("
                "id bigint primary key, body jsonb not null)"
            )
            rows, answer = await asyncio.gather(
                first.execute("select count(*) from memory"),
                second.execute("select 42"),
            )
            print(rows.fetchone(), answer.fetchone())
        finally:
            await first.close()
            await second.close()


asyncio.run(main())
