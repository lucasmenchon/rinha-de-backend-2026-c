# Rinha 2026 Entrant

A C implementation prepared for the Rinha de Backend 2026 preview.

The service is split into a small TCP edge process and two scoring workers. The
edge accepts HTTP on port `9999` and forwards requests over Unix stream sockets.
Each scoring worker keeps the binary reference index mapped in memory and uses a
compact IVF search path with AVX2 distance checks.

## Layout

- `src/edge`: TCP listener and Unix-socket proxy.
- `src/score`: request handling and fraud-score decision flow.
- `src/transport`: socket and HTTP primitives.
- `src/core`: vector normalization, distance math, and KNN voting.
- `src/index`: index reader plus the build-time index generator.

## Runtime Knobs

- `RNH_LISTEN`: TCP port used by the edge process.
- `RNH_UPSTREAMS`: comma-separated Unix socket paths for scoring workers.
- `RNH_BACKLOG`: TCP listen backlog.
- `RNH_INDEX`: path to the generated binary index.
- `RNH_SOCK`: Unix socket path exposed by one scoring worker.
- `RNH_NPROBE`: number of IVF lists scanned for each request.

## Local Run

```bash
docker compose up --build
```

The Docker image builds `bin/forge`, generates `/app/index.bin` from
`resources/references.json.gz`, and copies only the runtime binaries plus the
generated index into the final image.
