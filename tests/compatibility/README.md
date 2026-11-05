# Original writer integration

Run after the normal EdgeEcho Release build (native Linux):

```bash
python3 tests/compatibility/validate.py \
  --storage-source '/home/u-yoimiya/文档/EdgeSense Replay'
```

The reference writer source is read only. Its CMake build, bridge binary, fixture,
logs and result all reside in EdgeEcho/build-compat. Before/after SHA256 values
verify the reference CMake/source/header/SQLite files did not change. Existing
uncommitted changes in the reference checkout are preserved.

The bridge writes 3000 scalar records, including UTF-8 metadata, and a diagnostic
through the actual Store API, closes it normally, then reads them using EdgeEcho.
It checks all identifiers, timestamps, scalar presence, metadata entry count,
keyset pagination, latest event and min/max plot counts. A fresh CLI process
also inspects the file and verifies source hash and sidecar absence.

These are simulated records written through the real storage implementation,
not physical-sensor acquisition or a Windows runtime acceptance test. The regular
EdgeEcho build/tests have no dependency on the reference source or this bridge.
