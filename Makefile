# iperf-amigaos4 — Minimal iperf3-wire-protocol-compatible client for AmigaOS 4.1 PPC.
#
# Runs against a stock `iperf3 -s` server on Linux/macOS. TCP-only,
# single-stream, forward-direction (client sends). Enough to measure
# real network throughput of a SANA-II driver + Roadshow stack.
#
# Cross-compiled inside walkero/amigagccondocker:os4-gcc11-arm64.
# Invoke via scripts/build.sh (which drives the container); this
# Makefile is meant to run inside the container.

CC     = ppc-amigaos-gcc
STRIP  = ppc-amigaos-strip

CFLAGS = -mcrt=newlib -mhard-float -O2 -mcpu=440 -Wall -Wextra \
         -D__PPC__ -D__USE_OLD_TIMEVAL__ \
         -I./include

# -lauto opens interfaces (IExec, IDOS, bsdsocket) on first call.
LDFLAGS = -lauto

BUILD = build
BIN   = $(BUILD)/iperf3

SRCS = src/iperf3.c src/cjson.c
OBJS = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all clean

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/cjson.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	$(STRIP) --strip-all $@ 2>/dev/null || true

clean:
	rm -rf $(BUILD)
