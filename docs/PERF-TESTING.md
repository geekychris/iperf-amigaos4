# Performance testing on AmigaOS 4 — what actually works

The short version: **~40 Mbit/s** guest→host TCP is achievable on
QEMU sam460ex + Bill Borsari's `virte1000.device` + Roadshow, but
only from a fresh guest reboot. This document explains why,
what we tried, and how to get a real number.

## The number

Guest OS4.1 FE on QEMU sam460ex (`walkero/amigagccondocker` build
toolchain, macOS host on Apple Silicon), Bill Borsari's e1000
driver (`geekychris/amiga-e1000-driver`, `virte1000.device 0.2`),
Roadshow TCP stack, SLIRP `-netdev user`:

**~40 Mbit/s** first-shot guest→host TCP with 64 KB blocks.

Verified with both:
- `pyperf.py --client` (Python 3.12 on `python-os4`)
- `DH1:iperf3 -c ... --raw` (this project, C, plain newlib+bsdsocket)
- `DH1:iperf3 -c ... --raw --direct` (this project, C, direct `ISocket->send()`)

All three hit the same ~40 Mbit/s number. They also all degrade
the same way — see below.

## The degradation

Same setup, back-to-back tests without rebooting:

| test | client | throughput |
|---|---|---|
| 1 | our C `--raw` | 41.09 Mbit/s |
| 2 | our C `--raw` | 31.82 Mbit/s |
| 3 | `pyperf.py --client` | 2.80 Mbit/s |
| 4+ | anything | pegs at 2–3 Mbit/s |

The drop-off is real and reproducible. Cleared by a QEMU
reboot. Not cleared by restarting the host-side server or by
restarting devbench.

## Diagnosing the culprit — is it the driver?

We tested this with `teststress` (from Bill's amiga-e1000-driver
tree), which bypasses Roadshow entirely and calls `virte1000.device`
directly via `IExec->DoIO` in a tight loop. Same guest state,
same "run it 5 times back-to-back" pattern:

| run | elapsed | pps |
|---|---|---|
| 1 | 76.9 s | 2600 |
| 2 | 56.9 s | 3510 |
| 3 | 51.7 s | 3866 |
| 4 | 51.7 s | 3866 |
| 5 | 51.7 s | 3870 |

**Opposite pattern.** The driver actually *warms up* on the first
run and then stabilizes at exactly 3870 pps across three
consecutive 200,000-packet runs, no drift. Total 1,000,000
packets, zero failures. That's Bill's driver exonerated.

(The 3870 pps ceiling is the AmigaOS synchronous-DoIO round-trip
latency per packet — teststress waits for the driver task to
reply before sending the next. Roadshow's TCP stack pipelines
with `SendIO` async which is how it reaches ~40 Mbit/s despite
this per-request-latency limit.)

## Where the TCP degradation lives

By elimination, above the driver, in the socket / TCP layer.
Most likely: **Roadshow's TCP control-block pool filling with
TIME_WAIT-parked connections.** Every closed TCP connection sits
in TIME_WAIT for the 2×MSL period — Roadshow's default is on the
order of minutes. Once the pool is full, new TCP `connect()` calls
either wait or fail; existing connections' ACK handling slows down
as the stack churns TIME_WAIT tracking.

Consistent with the observed pattern:
- Only TCP-connection-cycle churn is affected. Individual
  connections stay fast for their full lifetime — one 30-second
  `-t 30` iperf3 run holds a steady 40 Mbit/s.
- pyperf and iperf-amigaos4 degrade equally (they both open a
  new TCP connection per test).
- teststress (no TCP) is stable.

We haven't yet confirmed with `netstat -an` counting TIME_WAIT
entries — that's the smoking-gun test if you want one. Add it
around a 5-test batch:

```
DH1:iperf3 -c 192.168.100.2 -p 17999 -t 5 -l 65536 --raw    # test 1
netstat -an | grep TIME_WAIT | wc -l                          # count after
DH1:iperf3 -c 192.168.100.2 -p 17999 -t 5 -l 65536 --raw    # test 2
netstat -an | grep TIME_WAIT | wc -l
# ... etc.
```

If TIME_WAIT count grows in step with the degradation, that's the
answer.

## What we tried that didn't help

Extensive elimination during the initial investigation
(before we realized both clients were equally affected):

- **Block size:** 4 KB, 16 KB, 64 KB, 128 KB — all give the
  same throughput in a given state.
- **`SO_SNDBUF` / `TCP_NODELAY`:** setting or clearing has no
  effect. Roadshow returns `SO_SNDBUF = 33580` regardless of
  what you request.
- **Roadshow config additions** (GATEWAY, DNSSERVER, routes
  file): all fine. Full config reaches 40 Mbit/s.
- **Removing per-iteration loop overhead** (`printf`, `clock()`):
  no effect.
- **`--direct` ISocket->send()** instead of libc `send()`:
  identical throughput.
- **`-D__USE_INLINE__`** in CFLAGS: makes the client hang
  because `-lauto` doesn't wire up the interface pointers the
  way inline sites expect. Not a perf question; incompatible.

## Testing recipe for real numbers

Always:

1. Fresh QEMU boot (`amiga_mcp/scripts/start-qemu-os4.sh`)
2. Wait for bridge heartbeat
3. Start server on host (`iperf3 -s -p 17999 --forceflush &`)
4. **Take the FIRST test result.**

Or use `amiga_mcp/scripts/perf-test.sh` which does the
server-launch + guest-launch dance for you (with pyperf.py by
default; adapt for iperf3).

## Bill's tools referenced here

- `teststress.c` — https://github.com/geekychris/amiga-e1000-driver
  — patched during this project to accept Ctrl-C (previously it
  ran to completion regardless of break signals, so a mis-typed
  large packet count locked the machine).
- `pyperf.py` — same repo, pure-Python 3 iperf-like tool. Copy
  bundled into this project's sibling `amiga_mcp/scripts/pyperf.py`
  for convenience.
