# Versioning policy

PostGamma uses one public product version and keeps implementation compatibility
identities separate. This prevents a PostgreSQL upgrade, C ABI revision, or
bundled-extension update from being mistaken for the product version.

## Public version format

Every published version uses exactly one of these forms:

| Phase | Format | Example |
| --- | --- | --- |
| Alpha | `X.Y.ZaN` | `0.1.0a1` |
| Beta | `X.Y.ZbN` | `0.1.0b1` |
| Release candidate | `X.Y.ZrcN` | `0.1.0rc1` |
| Stable | `X.Y.Z` | `1.0.0` |

The format is a strict subset of PEP 440. Public releases never use epochs,
date versions, `.devN`, `.postN`, or local `+label` suffixes. Prerelease serials
start at 1.

The Git tag is always `v` followed by the exact product version. For example,
version `0.1.0rc2` must be built from tag `v0.1.0rc2`. A tag/version mismatch
stops the release before compilation.

## What each number means

Before 1.0:

- increment the minor component for a planned public API or data-boundary
  change;
- increment the patch component for a compatible correction or feature set;
- advance `aN` to `bN`, then `rcN`, only as release confidence increases; and
- document direct cluster compatibility explicitly—never infer it from a
  matching `0.x` version.

At and after 1.0, PostGamma follows semantic-versioning intent: major for a
breaking public contract, minor for compatible features, and patch for
compatible fixes.

## Independent compatibility identities

| Identity | Current value | Purpose |
| --- | ---: | --- |
| Product version | `{{ POSTGAMMA_VERSION }}` | Python distribution, release, and SDK archive |
| PostgreSQL major | 19 | On-disk cluster format and PostgreSQL behavior baseline |
| C ABI | 1.3 | Native structure, symbol, and capability contract |
| Python public API baseline | 1 | Checked inventory of exported Python names |
| pgvector | 0.8.6 | Bundled extension implementation |

A future build can move to PostgreSQL 20 without pretending that the product
version is `20`. Conversely, a product patch release can retain PostgreSQL 19
and C ABI 1.3.

## One authoritative source

The repository root `VERSION` file is authoritative. The Makefile, Python wheel
builder, C build identity, release workflow, static SDK filename, and website
all consume it. Frozen API metadata and the current release note are checked
mirrors because they require deliberate review when a version changes.

Maintainers validate the complete identity with:

```bash
make version-check
```

The gate verifies the exact version grammar, release phase, `v` tag when one is
present, PostgreSQL build ID, Python baseline, C build composition, and current
release note. The product version cannot be overridden on a `make` command
line.

## Artifact identity and immutability

One version identifies one immutable set of bytes. A release contains five
CPython wheels, one static SDK archive, notices, `release-manifest.json`, and
`SHA256SUMS`. If any published byte must change, publish a new version; never
replace an asset under an existing version.

The release workflow first creates a draft and uploads the exact artifacts that
passed validation. Publication requires the protected `github-release`
environment. PyPI Trusted Publishing then downloads and re-verifies those same
wheel bytes from the published GitHub Release.

## Repository publication controls

Before the first public tag, the repository owner must complete four one-time
settings:

1. create a `github-release` environment with required reviewers;
2. enable immutable releases in the repository release settings;
3. register the `pypi` environment as a Trusted Publisher for the `postgamma`
   PyPI project; and
4. set the repository variable `POSTGAMMA_PYPI_PUBLISH_ENABLED=true` only after
   the PyPI project name and publisher are under the release owner's control.

A manual workflow run builds and validates the full bundle but cannot publish
it. Only a matching `vX.Y.Z...` tag stages a draft. The protected environment
approval publishes that draft, and the resulting release event is the only
trigger accepted by the PyPI workflow.

## Cluster upgrades

Matching product versions do not override PostgreSQL's physical-format rule.
Crossing a PostgreSQL major always requires logical dump and restore. During
the alpha series, direct compatibility between two PostGamma builds is promised
only when the destination release note says so. Keep the original cluster and
logical archive until restore verification is complete.

[Release history](index.md) · [Logical backup and restore](../python/management.md)
