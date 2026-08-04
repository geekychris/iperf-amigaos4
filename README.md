# iperf-amigaos4

A minimal `iperf3`-wire-protocol-compatible **client** for AmigaOS 4.1 PPC.
Runs against a stock `iperf3 -s` server on Linux / macOS to measure real
TCP throughput of a SANA-II network driver + Roadshow TCP stack.

Written because upstream `iperf3` (esnet/iperf) is pthread-mandatory
since 3.x, and AmigaOS 4 doesn't have POSIX threads. Rather than write
a pthread → OS4 process shim (2–3 weeks of work), this project speaks
the iperf3 wire protocol directly using a single OS4 task and
Roadshow's `bsdsocket.library`.

## Scope

**Supported:**
- TCP client only, forward direction (guest sends to host)
- `-c <host>` `-p <port>` `-t <seconds>` `-l <blksize>`
- Single stream
- Real iperf3 wire protocol (works against any `iperf3 -s`)
- `--raw` mode: skip the iperf3 protocol; open TCP and blast. Useful
  against `pyperf.py --server` or any dumb-recv server.
- `--direct` mode (implies `--raw`): call `ISocket->send()` from an
  explicit `IExec->OpenLibrary("bsdsocket.library", 4)` instead of
  going through newlib's libc socket wrapper. Diagnostic — should
  be same throughput as libc.

**Not supported (yet):**
- Reverse mode / UDP / multi-stream (`-P N`)
- Server mode
- SCTP / auth / bidirectional
- JSON output on the client side (the iperf3 server prints the number)

## Build

Requires Docker + the walkero AmigaOS 4 GCC 11 image (auto-selected
for your host arch — arm64 image on Apple Silicon, amd64 on x86 hosts).

```
./scripts/build.sh
```

Output: `build/iperf3` (PPC ELF, statically linked, stripped, ~80 KB).

## Deploy + run

Push to guest `DH1:` via `amiga_mcp` devbench:

```
curl -sf -X POST http://localhost:3000/api/transfer \
  -H 'Content-Type: application/json' \
  -d '{"source":"'$(pwd)'/build/iperf3","dest":"DH1:iperf3","direction":"push"}'
```

On host: start server (pick a port that isn't hostfwd'd by QEMU):

```
iperf3 -s -p 17999 --forceflush
```

On guest:

```
DH1:iperf3 -c 192.168.100.2 -p 17999 -t 10
```

`192.168.100.2` is the SLIRP gateway which doubles as a host proxy
in `amiga_mcp`'s default QEMU config. `--forceflush` prevents
iperf3 -s from buffering its stdout when redirected to a file.

## Measuring throughput correctly

**Always start from a fresh QEMU boot for benchmark numbers.**

We observed that after ~2–3 successive TCP tests the guest's TCP
throughput degrades from ~40 Mbit/s to ~2–3 Mbit/s. The pattern
affects both this client and pyperf.py equally, so it isn't a
client-specific bug. Most likely explanation: Roadshow's TCP
control-block pool filling with TIME_WAIT-parked connections
(default TIME_WAIT is minutes long). See `docs/PERF-TESTING.md`
for the full write-up and the diagnostics we ran.

Recipe for a real number:

```
# host
./scripts/start-qemu-os4.sh          # from amiga_mcp
# wait for guest to boot + bridge to come up
iperf3 -s -p 17999 --forceflush &

# guest — take the FIRST result
DH1:iperf3 -c 192.168.100.2 -p 17999 -t 10
```

Expected: ~40 Mbit/s on Bill Borsari's `virte1000.device` over
QEMU sam460ex e1000 emulation via SLIRP.

## Files

- `src/iperf3.c` — client, wire protocol + state machine + blast loop (~500 lines)
- `src/cjson.c`, `include/cjson.h` — bundled cJSON from esnet/iperf
- `include/iperf_config.h` — stub for cjson's unconditional include
- `Makefile` — cross-compile via walkero:os4-gcc11
- `scripts/build.sh` — docker wrapper
- `docs/PERF-TESTING.md` — full profiling story, what we learned,
  known caveats around SLIRP/TCP state degradation

## License

- Client code (`src/iperf3.c`): BSD 3-Clause (inherits from esnet/iperf
  which the wire protocol is derived from).
- `src/cjson.c`, `include/cjson.h`: taken verbatim from esnet/iperf
  (originally MIT-licensed cJSON).
- `LICENSE.iperf`: the esnet/iperf BSD 3-Clause license.
