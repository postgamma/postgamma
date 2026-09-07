# Security policy

PostGamma runs PostgreSQL and bundled native extensions inside the host
application process. Read the public [security model](docs/concepts/security.md)
before evaluating an issue.

## Supported versions

PostGamma has not published a stable release. Security fixes currently target
the latest commit on the maintained release branch and will be identified in
the next release notes.

## Report a vulnerability privately

Use the repository's private vulnerability-reporting interface. On GitHub,
open the repository's **Security** tab and select **Report a vulnerability**.

Include the affected revision or package version, platform, PostgreSQL major,
minimal reproduction, impact, and whether untrusted SQL, a cluster directory,
an extension, or a resource pack is involved. Do not attach sensitive cluster
contents unless the maintainers explicitly request a safe transfer.

Do not open a public issue for an undisclosed vulnerability. If GitHub private
reporting is unavailable, open a public issue containing no vulnerability
details and ask the maintainers to provide a private contact channel.

## Scope

Reports about memory safety, cross-instance isolation, unexpected host process
or signal effects, arbitrary code or file access outside the documented trust
boundary, unsafe artifact loading, or durability violations are in scope.
General PostgreSQL behavior that is unchanged upstream should also be reported
to the PostgreSQL project when appropriate.
