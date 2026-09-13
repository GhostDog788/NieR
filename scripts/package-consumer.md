# Sela compiler-only distribution prototype

The distribution documentation lives in [Sela compiler-only distribution prototype](../docs/reference/compiler-distribution.md), inside the `docs/` vault.
It covers native x86-64/i686 SDKs, shared versus static LLVM, device-only archive dependencies, stripped delivery receipts, and the current qualification checkpoint.

Package an already completed matching SDK/build into a new output directory:

```bash
bash scripts/package-consumer.sh build/consumer-x86_64 \
  artifacts/selac-x86_64 .sdk/consumer/x86_64
```

The source SDK and build products are not stripped or overwritten.
The staged compiler/tool copies are release-stripped, and `payload.sha256` records their final delivered bytes.
The managed runtime and compiler-rt are retained unchanged.

Packaging prints a size report; the same read-only reporter can inspect an existing package:

```bash
python3 -B scripts/bundle-size.py artifacts/selac-x86_64
python3 -B scripts/bundle-size.py artifacts/selac-x86_64 \
  --compare /absolute/path/to/baseline --json --compressed
```

Use the reference's separate measured checkpoints rather than assuming that a selected shared/static build configuration has already passed qualification.
