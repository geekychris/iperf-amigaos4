# iperf-amigaos4

A minimal `iperf3`-wire-protocol-compatible **client** for AmigaOS 4.1 PPC.
Runs against a stock `iperf3 -s` server on Linux / macOS to measure real
TCP throughput of a SANA-II driver + Roadshow TCP stack.

Written because the upstream `iperf3` codebase (esnet/iperf) requires
POSIX pthreads which AmigaOS 4 doesn't have. A full port would need a
pthread→OS4 process shim (2–3 weeks of work). This project instead
speaks the iperf3 wire protocol directly with plain OS4 tasks, using
Roadshow's `bsdsocket.library`.

## Scope

**Supported:**
- TCP throughput test
- Client mode (forward direction — guest sends to host)
- `-c <host>` `-p <port>` `-t <time>`
- Single stream

**Not supported (yet):**
- Reverse mode
- UDP
- Multi-stream / `-P N`
- Server mode
- SCTP / auth / bidirectional
- JSON output on our side (the server prints the number)

## Build

Requires Docker + the walkero AmigaOS 4 GCC 11 image (auto-selected
for your host arch).

```
./scripts/build.sh
```

Output: `build/iperf3` (PPC ELF, statically linked, stripped).

## Deploy + run

Push to guest DH1: via `amiga_mcp` devbench:

```
curl -sf -X POST http://localhost:3000/api/transfer \
  -H 'Content-Type: application/json' \
  -d '{"source":"'$(pwd)'/build/iperf3","dest":"DH1:iperf3","direction":"push"}'
```

On host: start server (pick a port that isn't hostfwd'd by QEMU):

```
iperf3 -s -p 17999
```

On guest:

```
DH1:iperf3 -c 192.168.100.2 -p 17999 -t 10
```

(`192.168.100.2` is the SLIRP gateway which doubles as the host proxy
in `amiga_mcp`'s default QEMU config.)

## License

- Client code (`src/iperf3.c`): BSD 3-Clause (inherits from esnet/iperf
  which the wire protocol is derived from).
- `src/cjson.c`, `include/cjson.h`: taken verbatim from esnet/iperf
  (originally MIT-licensed cJSON).
- `LICENSE.iperf`: the esnet/iperf BSD 3-Clause license.
