# Security policy

Sela is pre-alpha software, not a hardened production compiler or sandbox.
Development and fixes target the current `main` branch; older commits and pre-alpha artifacts have no maintained compatibility or security-support guarantee.
No response deadline or supported release series is promised.

Artifact validation, input bounds, digest checks, and output staging are correctness and robustness measures.
They do not implement SESela (SES), the separately planned security platform for trusted publishers, signed code, and executable-memory enforcement.
Do not rely on the current toolchain to safely isolate hostile compiler inputs.

## Report a suspected vulnerability privately

Do not put exploit details, sensitive inputs, private source, credentials, or unpublished vulnerability information in a public issue or pull request.

1. Visit the repository's [Security tab](https://github.com/GhostDog788/Sela/security).
2. If **Report a vulnerability** is available, use it to submit a private report.
3. If private reporting is unavailable, open a [new issue](https://github.com/GhostDog788/Sela/issues/new) containing only a non-sensitive request for a private reporting channel, such as “How can I share a security report privately?”
   Do not include the affected component, a reproducer, logs, or other technical vulnerability details there.
   Wait until a private channel has been arranged before sharing them.

Private GitHub reporting depends on repository settings; this file does not enable it or imply that it is already enabled.
The Sela GitHub URL is the intended renamed destination, not an already completed repository-settings change.
Until that rename is applied, use the **Security** or **Issues** tab of the existing repository you are viewing if the links above are unavailable; the same private-reporting rules apply.

## What to include once a private channel is available

- The affected commit and whether you have local modifications.
- Host OS/architecture, SDK/tool versions, and relevant target settings.
- A minimal reproducer and the impact you observed, shared only when you have permission to provide the material.
- Whether the issue involves publication, artifact parsing, native compilation, build integration, or packaging.

Review logs and captures before sending them, even privately.
Producer-side captures can include application source details, debug information, and local paths.
Do not test a suspected issue against systems or data you are not authorized to use.

Ordinary non-security bugs and documentation corrections belong in the public issue tracker.
If you are unsure whether a finding is security-sensitive, use the private-channel process above first.
