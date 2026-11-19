# Replay writer interoperability

Run from the EdgeEcho project directory on Linux.

```bash
python3 tests/compatibility/validate.py \
  --storage-source '/home/u-yoimiya/文档/EdgeSense Replay'
```

EdgeEcho and the fixture writer link the same Replay Core build. CMake outputs,
the fixture, logs and result stay under `build-compat`. The runner checks the
Replay checkout's source hashes before and after the build.

The fixture writes 5,000 scalar records with UTF-8 metadata, a 48-byte XYZ feature
scan, an overlapping waveform and a confirmed diagnostic through Replay's
public Store API. It checks raw record pagination and metadata, the latest event
revision and confirmation, XYZ feature channels, and the half-open window for
the waveform plot and probe. A fresh CLI process inspects the database, after
which the runner checks the source hash and sidecar absence.

This run exercises simulated records on the native Linux build.
