# GitHub presentation and maintenance

[Documentation home](../README.md) · [Project reference](README.md) · [Current toolchain status](toolchain-status.md)

The root README is the short public introduction.
Detailed compiler behavior belongs in the guides and reference pages, with visible links from that introduction.
Keep the distinction between the NieR format's general vision and the qualified C/Linux pre-alpha explicit.

## Repository settings to finish on GitHub

Committing files does not configure every repository setting.
After reviewing and pushing the changes to `GhostDog788/NIER`, complete these steps with a repository administrator account.
The repository is already public; no visibility or collaborator-permission change is needed to apply the Apache-2.0 license.

1. Open **Settings → General → Social preview**, choose **Edit → Upload an image**, and upload `assets/social-preview.png` from the checkout.
   It is 1280 × 640, below GitHub's 1 MB limit, and uses the supplied NieR logo.
   See [GitHub's social-preview instructions](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/customizing-your-repositorys-social-media-preview).
2. Open **Settings → Advanced Security** and enable **Private vulnerability reporting** if it is not already enabled.
   Confirm that **Security → Advisories → Report a vulnerability** is available to reporters.
   Follow [GitHub's current private-reporting instructions](https://docs.github.com/en/code-security/security-advisories/working-with-repository-security-advisories/configuring-private-vulnerability-reporting-for-a-repository) if the setting's location changes.
   Until enabled, the root `SECURITY.md` provides a non-sensitive request-for-contact fallback; do not ask users to post vulnerability details publicly.
3. In the repository's **About** panel, use the description and optional topics below.
   These are repository metadata, not fields set by the README.
   GitHub documents the [About-panel topic controls](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/classifying-your-repository-with-topics).
4. Check the first **CI** workflow run in **Actions** after pushing.
   Ensure Actions is allowed for the repository if no run starts.
   The README badge reports that workflow's actual state; adding a workflow file or passing local tests does not establish a passing hosted run.

Suggested description:

```text
Architecture-neutral NieR artifacts, compiled into ordinary native binaries. An open-source, pre-alpha C/LLVM toolchain.
```

Suggested topics:

```text
compiler llvm mlir intermediate-representation ahead-of-time clang c linux
```

These describe the current project without claiming complete Rust, Go, ARM, or security-platform support.

## Branding assets

The repository owns these presentation files:

| Repository path | Purpose |
| --- | --- |
| `assets/nier-logo.png` | Original user-supplied wordmark, reused unchanged. |
| `assets/nier-flow.svg` | Editable diagram of publication, the independent artifact, and target-side compilation. |
| `assets/social-preview.svg` | Editable social-card layout with the original wordmark embedded for self-contained rendering. |
| `assets/social-preview.png` | Rendered image for GitHub's social-preview upload. |

The diagram and social card use an explicit dark background and a single teal accent, so their labels remain legible in both light and dark surrounding pages.
They are code-native SVG assets; the project logo is not redrawn or generated.
Do not change current-support labels into promises that the compiler has not demonstrated.

To render the social PNG after editing its SVG, use FFmpeg with its `librsvg` decoder and DejaVu Sans/Mono fonts installed:

```bash
bash scripts/render-branding.sh
```

This overwrites only the generated `assets/social-preview.png`.
Inspect the result for clipped text and verify the dimensions and file size before uploading it.
If the wordmark changes, update both the original logo and the image embedded in the social SVG; the embedded copy deliberately has no external file dependency.

## Refresh the real Hello recording

The README animation comes from real interactive Bash output, not illustrative compiler logs.
The recorder uses a fresh temporary copy of the Hello C sources and the existing stock-Clang integration and `nierc` binaries.
It does not rebuild those tools or copy application build caches.

The optional recording tools are Python 3, Pillow, DejaVu Sans/Mono fonts, and the host `file` utility.
They are presentation-maintenance dependencies, not prerequisites for compiling or using NieR.
Build a coherent SDK and pre-alpha toolchain first; do not relink it while recording.

```bash
bash scripts/record-hello-demo.sh
```

This refreshes `assets/hello-demo.gif`, `assets/hello-demo.cast`, `assets/hello-demo.txt`, and `assets/hello-demo.provenance.json`.
The plain-text transcript provides the same commands and output without animation.
The cast preserves actual terminal-event timestamps; the animation includes intentional typing and reading pauses at real-time playback speed.
Neither duration is a compilation-performance benchmark.

The recording's prepared shell puts the SDK tools and `build/prealpha` on `PATH`, and supplies `NIER_CONFIG` privately.
The displayed compiler commands are stock `clang` and the separate `nierc`; the preparation is not a new public compiler entry point.
The recording rejects personal checkout, home, or SDK paths in its output.
A generated `/tmp/nier-demo-…` output pathname is ordinary compiler output, not a leaked private workspace.
Review the transcript before publishing, even when the automated guard passes.

## Maintain honest CI and support information

The workflow is `.github/workflows/ci.yml`; its matching local check is:

```bash
bash scripts/ci-smoke.sh
```

It expects the standard publisher-enabled `build/prealpha` build and checks that all nine named smoke tests exist before running them.
An optional argument selects another configured build directory.
It does not configure or compile NieR, run the full corpus, or claim security/performance acceptance.
The hosted workflow bootstraps and builds on Ubuntu 24.04 with two build jobs, a 30-minute job limit, and bounded individual steps and tests.

The SDK archive cache is keyed by the package lock; every archive is still hash-verified during bootstrap.
Extracted tools, receipt files, build outputs, and credentials are not cached.
Pull requests restore caches; only successful pushes to `main` save new ones.
Action versions are pinned to reviewed commit hashes and the workflow token has read-only repository contents access.

When the package lock changes, verify availability from the [dated Ubuntu snapshot](development-sdk.md), review the host dependencies, and validate a fresh build.
Local smoke success does not prove a fresh GitHub-hosted environment succeeded; inspect the actual run before making that claim.

The root `CONTRIBUTING.md`, `SECURITY.md`, issue forms, and pull-request template provide project entry points.
Keep their links, qualified scope, and reporting instructions synchronized with the guides.
The Apache-2.0 license applies to project-owned work; third-party notices remain separate and must be retained in distributions.
