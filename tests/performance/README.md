# Synthetic CLI performance run

Run from the EdgeEcho directory after a Release build:

```sh
python3 tests/performance/run.py --output build/performance --cli build/edgeecho_cli
```

After rebuilding the CLI, rerun against the same workload without rewriting
the database:

```sh
python3 tests/performance/run.py --output build/performance --cli build/edgeecho_cli --reuse
```

Reuse validates the runner marker, workload counts and sidecar absence, then
opens the synthetic database read-only. Every benchmark and fingerprint is
measured again; earlier timing/RSS output is never reused.

The runner requires Python 3.9+, Linux and GNU `/usr/bin/time`. It creates only
workspace-local outputs and refuses unrelated nonempty output directories.
Its own output directory may be reused. It creates a synthetic demo with
200,000 scalar records, then appends 250 one-second IIO vibration blocks of
32,768 scans each (8,192,000 points for the selected X channel).

Three fresh CLI processes measure the complete waveform, complete scalar
series and the final scalar second. Each result records CLI plot time, total
process time and per-process peak RSS. Filesystem cache is uncontrolled; these
are sequential host measurements. They do not establish hardware acquisition
accuracy, cold-cache performance or Windows runtime performance.

The runner records database hashes before/after, source-sidecar absence,
machine information, executable/source fingerprints, actual counts and errors
in `docs/evidence/performance.json` and the output directory's `report.json`.
It leaves the generated synthetic database and raw measurement logs in the
output directory for inspection. Budget failure is reported as failure with
its actual error; the default 15-second query timeout is not bypassed.

The full scalar scan also catches inefficient keyset SQL. Each subsequent
page explicitly binds `ts >= max(query.start, cursor.timestamp)` as the index
range lower bound, while `(ts,seq) > (cursor.timestamp,cursor.sequence)` keeps
equal-timestamp rows unique. Leaving only the original range start as a
separate bound can make SQLite repeatedly visit the already-consumed prefix.
The final implementation retained all 200,000 records while reducing this
host's observed scalar plot time from about 7,052 ms to 550 ms. These are two
individual sequential measurements with uncontrolled filesystem cache, not a
general performance guarantee. The latest exact results and fingerprints are
in `docs/evidence/performance.json`.

## Current 0.3 browsing benchmark

After the final Release build, use the existing runner-owned workload:

```sh
./build/edgeecho_browsing_bench build/performance/performance.db 100
```

The executable validates fixture counts and result consistency on every repetition.
It measures full waveform, full scalar, tail-window, shared XYZ and original-sample
probe latency from one offline connection. P50/P95 use nearest-rank percentiles;
100 repetitions are required for the published current report. This is query
latency with an uncontrolled warm cache, excluding GUI painting and process startup.
Current evidence is `docs/evidence/current-performance.json`; the older
`performance.json` is a historical measurement of different source.
