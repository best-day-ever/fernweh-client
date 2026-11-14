# Fernweh Client

The shared macOS, Windows and Linux Client for [BDE Fernweh](https://github.com/best-day-ever/fernweh),
an enhanced downstream fork of [PLANK Client](https://github.com/instinctual/plank-client).
This is public ALPHA source; publication does not announce a binary release.
The parent [publication receipt](https://github.com/best-day-ever/fernweh/blob/main/docs/fernweh/review.md) records the history audit.

Build from the parent Fernweh repository's pinned gitlink, native transport,
dependency archives and patches. Standalone qmake source is not a replacement
for that build contract. Use the parent's [platform instructions](https://github.com/best-day-ever/fernweh/blob/main/docs/fernweh/build.md)
and [configuration guide](https://github.com/best-day-ever/fernweh/blob/main/docs/fernweh/configuration.md).

| Platform | Fork scope |
| --- | --- |
| macOS 15+, Apple Silicon | SDK 27+/Qt 6.10.2; Fernweh SDL session path |
| Windows x64 | MSVC 2022/Qt 6.10.2; ALPHA Client/MSI tooling |
| Ubuntu 26.04 x86-64 | Inherited PLANK Qt/Wayland path; new-fork acceptance required |

Choose `FERNWEH_BROKER_ENABLED=0` (default) for direct-only access, or `1` to
compile broker sign-in, device sessions and relay support. The disabled executable
omits broker backends/UI, enrollment, the passkey helper and broker updates.

`FERNWEH_BROKER_HOST`, `FERNWEH_BROKER_PORT`, `FERNWEH_BROKER_PINS`,
`FERNWEH_PASSKEY_RP_ID` and `FERNWEH_UPDATES_ENABLED` configure a studio build.
`deployment/fernweh.pri` validates the environment with Python 3 before compilation.
Generic builds contain no operator endpoint, trusted broker pin or enabled updates.
No BDE service, signing key or private deployment checkout is needed to compile.

See the parent's [enhancement map](https://github.com/best-day-ever/fernweh/blob/main/docs/fernweh/enhancements.md)
for broker sign-in, device sessions, onboarding, collaboration, clipboard,
Windows work and display/lifecycle changes. Feature support depends on the
Client/Host pair; tests and compilation do not establish hardware qualification.

For development read the parent [AGENTS.md](https://github.com/best-day-ever/fernweh/blob/main/AGENTS.md)
and [shared Claude/Codex skill](https://github.com/best-day-ever/fernweh/blob/main/.agents/skills/fernweh-development/SKILL.md).
Keep component commits separate, push them before parent gitlinks, and use the
[upstream workflow](https://github.com/best-day-ever/fernweh/blob/main/docs/fernweh/upstreaming.md)
for focused contributions back to PLANK. Preserve PLANK/Moonlight Qt authorship,
commit history, copyright notices and the [GPLv3 license](LICENSE).
