# License

Copyright 2026 Shujie Zhang.

PostGamma-owned source code and documentation are licensed under the
[Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0). The complete
terms are in the repository's top-level `LICENSE` file and are included in
every release artifact.

PostGamma embeds and transforms PostgreSQL source code. PostgreSQL remains
covered by the PostgreSQL License, which is included as
`LICENSE.postgresql` in Python wheels and as
`licenses/LICENSE.postgresql` in the static C SDK archive.

The bundled pgvector source remains covered by its PostgreSQL License. Its
notice is included as `LICENSE.pgvector` in Python wheels and as
`licenses/LICENSE.pgvector` in the static C SDK archive.

## Artifact license files

| Artifact | PostGamma terms | PostgreSQL terms | pgvector terms |
| --- | --- | --- | --- |
| Source repository | `LICENSE`, `NOTICE` | `licenses/LICENSE.postgresql` | `licenses/LICENSE.pgvector` |
| Python wheel | `LICENSE`, `NOTICE` | `LICENSE.postgresql` | `LICENSE.pgvector` |
| Static C SDK archive | `licenses/LICENSE.postgamma`, `licenses/NOTICE` | `licenses/LICENSE.postgresql` | `licenses/LICENSE.pgvector` |

Every artifact also includes `THIRD_PARTY_NOTICES`, which identifies the
upstream source and pinned revision for each bundled component.

Additional bundled libraries or extensions may carry their own notices. Check
the release manifest before redistributing an artifact. A static application
link does not remove the obligation to preserve the license files supplied
with the release.

This page summarizes the distribution layout; the license texts control.
The official project website is [postgamma.com](https://postgamma.com).
