# Blog editorial notes

These are the first three PostGamma blog posts, published as a batch on
October 31, 2026. They live in `docs/blog/posts/`, with `draft: false` in
their metadata. Both the normal production build and the local preview
include all three articles.

| Writing order | Publication time on October 31 (UTC) | Article | Purpose |
| --- | --- | --- | --- |
| 1 | 13:19:00 | [Introducing PostGamma](../../docs/blog/posts/01-introducing-postgamma.md) | Introduce the project and a small Python example |
| 2 | 13:19:01 | [Threading PostgreSQL Without Editing Upstream Source Code](../../docs/blog/posts/02-threading-postgresql-without-editing-upstream-source-code.md) | Explain why upstream stays untouched and how PostGamma generates and checks changes |
| 3 | 13:19:02 | [A Repeated Check in PostGamma](../../docs/blog/posts/03-a-repeated-check-in-postgamma.md) | Explain short-query threading benefits, one repeated state check, and two separate historical comparisons |

The timestamps keep the launch batch in newest-first order: article 3,
then 2, then 1. The blog uses MkDocs Material's publication-time ordering,
including within categories. Do not pin these posts above newer articles.

## Voice

Use short sentences, common words, and first-person explanations. Keep
necessary technical terms and explain them when first used. The author's
own introduction is the source for the AI-assisted prototype and the
approximate 48-hour timeline. Do not add personal events or feelings.

## Source notes

The code baseline is `cc19919ef8aa1c6ccf4ddb87e9a555f6cac884cb`.

Article 1 follows `README.md`, `docs/getting-started/index.md`,
`docs/concepts/concurrency.md`, `docs/concepts/durability.md`, and
`docs/compatibility/limits.md`. The Python example uses the existing public
API and SQL pattern from the repository quickstart. The author supplied
the four-OS full-test result. Its archived evidence is in the native
validation checkout under
`build/remote-full-test/cloud-control/matrix-20261026T155113Z-copy-sync-fix`.

Public release status was checked on October 31, 2026:
[GitHub Releases](https://github.com/postgamma/postgamma/releases) had no
published release, and the [download page](https://postgamma.com/downloads/)
still directed readers to source builds. Check this status again before
publication.

Article 2 follows `docs/concepts/architecture.md`,
`docs/concepts/concurrency.md`, `manifests/ownership/backend-state-rules.json`,
`buildsys/materialize.py`, `buildsys/apply_replacements.py`,
`compiler/src/postgamma_cc.cpp`, `runtime/src/execution_context.c`, and
`.github/workflows/upstream-canary.yml`. The pipeline diagram is a simplified
view of the build. The main point is that the upstream checkout stays
untouched while a separate generated tree contains the changes. The time
zone example explains one ownership rule. This does not claim that the
generated program is unchanged or that new upstream versions need no work.

Article 3 focuses on the redundant table-pointer check after address caching
already existed. It is narrower than the initial idea of covering the whole
state-address caching change. This choice has a retained before/after patch,
six pairs of timing results, and an explicit validation report.

The short-query section adds the separate October 14 comparison from
`build/general-performance-20261014/` in the native validation checkout.
Its report is archived as `general-server-performance-results-2026-10-14.txt`
in the design archive. The retained `matrix-summary.json`, `matrix.json`,
source snapshot, identities, SQL, and method files match their recorded
SHA-256 hashes in `evidence-manifest.json`.

The [short-query evidence](../../docs/blog/evidence/short-query-concurrency-2026-10-14.json)
preserves all 21 workload/concurrency rows, paired results, method, settings,
and binary identities. The prepared point-query table shows 1, 4, and 16
persistent sessions. The server used four CPUs and the clients used two
separate CPUs. This is not the connection-churn test or the embedded worker
pool. Do not attribute its gains solely to the October 10 check removal or
claim that it isolates OS context-switch costs. The candidate included
several accumulated changes. Keep the low-concurrency and aggregation
regressions visible alongside the high-concurrency point-query gains.

The mechanism paragraph follows PostgreSQL's architecture documentation and
Linux v6.8 `arch/x86/mm/tlb.c`, especially the same-address-space path in
`switch_mm_irqs_off`. It does not claim that every process switch flushes
the TLB or that threads eliminate scheduling and socket costs.

The October 10 source is `thread-state-access-20261010/report.txt` in the
project's design archive, with the adjacent `evidence.tar.gz`.
The relevant archive members are:

- `notes/candidate-final.patch`;
- `notes/final-identity.json`;
- `diag-state/paired-a3/summary.json`;
- `diag-state/paired-a3/report.json`.

The accompanying [evidence JSON](../../docs/blog/evidence/state-access-check-2026-10-10.json)
preserves the timing summary and the two binary identities. The source
commit recorded for the final historical build is
`9e1461b2c94457cb8ed30ae599dcb30feeca734e`.

Keep these boundaries for the October 10 state-access comparison:

- Both columns compare historical threaded builds with the same build flags.
- The final build also contains a pointer initialization correctness fix.
- The table does not isolate the timing effect of one removed branch.
- The earlier native PostgreSQL control had different compiler flags; it is
  excluded from the article's numerical comparison.
- The internal-loop denominator is completed lookups, while each network
  operation includes a separate request.
- Lower instruction counts and median times are different measurements.
- The timing runs cover read-only work on tmpfs, not disk-write performance.
- These results do not describe current release performance or Wasm.

## Publication notes

Read future posts in the author's voice and adjust any phrases that feel too
formal. Recheck product status, source links, and dates at publication time.
Each cover has its own visual direction: a colorful printed illustration for
the introduction, layered blueprints for the source transformation, and a dark
debugging scene with an abstract flame graph for the performance story. These
are editorial illustrations, not measured results or exact architecture diagrams.
The covers appear in both the article list and article headers. The evidence
link is part of the website.
The [cover artwork notes](cover-art.md) record the asset paths and generation
prompts.
Keep the published `slug` fields unchanged so article URLs stay stable.
Future posts can use `draft: true` while editing; set `draft: false` when
they are ready to publish.
