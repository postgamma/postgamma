# Download PostGamma

Official binaries are published only after the complete release matrix passes.
GitHub Releases is the canonical home for static SDK archives, checksums, and
wheel mirrors. PyPI receives the exact same tested wheel bytes.

<div class="pg-download-status" id="release-status" markdown>

<span class="pg-card-label">CURRENT CANDIDATE</span>

## {{ POSTGAMMA_VERSION }} · {{ POSTGAMMA_RELEASE_STATE }}

The first public binary release is being prepared. Until its GitHub Release is
published, build from source or use an explicitly shared candidate artifact.
Do not treat an Actions artifact as an official release.

[Open GitHub Releases]({{ POSTGAMMA_RELEASES_URL }}){ .md-button .md-button--primary }
[Read the release notes](releases/{{ POSTGAMMA_VERSION }}.md){ .md-button }

</div>

## Python

Once the release is published, let `pip` select the wheel for the active
CPython interpreter:

```bash
python -m pip install postgamma
```

The initial matrix contains one `manylinux_2_28_x86_64` wheel for each of
CPython 3.10, 3.11, 3.12, 3.13, and 3.14. No source distribution is published
in this release, so installation fails instead of compiling an unverified local
kernel when no compatible wheel exists.

To install a wheel downloaded from GitHub Releases:

```bash
python -m pip install ./postgamma-{{ POSTGAMMA_VERSION }}-*.whl
```

[Installation details](getting-started/installation.md) ·
[Python quickstart](getting-started/index.md)

## Static C SDK

The native release asset is:

```text
postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64.tar.gz
```

It contains `libpostgamma.a`, public headers, `postgamma.pc`, a complete
resource pack, a compiled example, and all required license notices. Download
the archive and its adjacent checksum from the same release:

```bash
sha256sum --check \
  postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64.tar.gz.sha256
tar -xzf postgamma-sdk-{{ POSTGAMMA_VERSION }}-linux-x86_64.tar.gz
```

[Static C quickstart](getting-started/c-static.md) ·
[Build and link](c/build-and-link.md)

## Verify a release

Every GitHub Release contains:

| Asset | Purpose |
| --- | --- |
| `postgamma-*.whl` | Platform-specific CPython distributions |
| `postgamma-sdk-*.tar.gz` | Static C SDK and matching resource pack |
| `SHA256SUMS` | Digests for every published payload and the manifest |
| `release-manifest.json` | Version, PostgreSQL major, roles, sizes, and tested digests |
| `LICENSE`, `NOTICE`, and third-party notices | Redistribution terms and attribution |

Verify the complete downloaded set before redistribution:

```bash
sha256sum --check SHA256SUMS
```

Release assembly uses the hashes produced by the wheel and static-SDK test
gates. A release is rejected if an asset was rebuilt or changed after those
tests.

## Supported binary target

| Component | Initial target |
| --- | --- |
| Operating system | Linux |
| Architecture | x86-64 |
| libc | glibc 2.28 or newer |
| Python | CPython 3.10–3.14 |
| PostgreSQL cluster major | 19 |
| C delivery | Static SDK; versioned ABI 1.3 |

Developer builds made on newer Linux systems may require a newer glibc. Only
the official release matrix carries the `manylinux_2_28` compatibility claim.

## Source builds

The repository keeps PostgreSQL and bundled extension sources as submodules.
From a complete checkout:

```bash
git submodule update --init --recursive
make release-candidate JOBS=8
```

This produces a tested local wheel, static SDK, documentation site, and release
receipt. It does not upload or publish anything.

Read the [versioning policy](releases/versioning.md) before comparing builds or
upgrading a cluster.
