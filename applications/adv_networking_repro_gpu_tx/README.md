# DPDK split-TX backpressure failure reproducer

## Overview

High-level problem: DPDK split-TX does not safely enforce backpressure in the 2-segment GPU-backed path.

This reproducer was used to validate the TX hardening and partial-cleanup fix
for that path. With the current fix set, the full-speed 2-segment GPU case no
longer crashes. It can hit transient burst-pool exhaustion during
`DpdkMgr::get_tx_packet_burst()`, retry, and recover cleanly.

Current observed behavior:

- `2-seg GPU` succeeds at full speed with transient retry-based recovery
- `2-seg GPU` succeeds when gaps between bursts are added
- `2-seg CPU` succeeds at full speed
- `1-seg CPU` succeeds at full speed

Results in [FINDINGS.md](./FINDINGS.md).

## Problem

This reproducer exercises the DPDK split-TX backpressure edge case in the
2-segment GPU-backed path and validates that recovery is now fail-safe.

At a high level:

- baseline `repro.yaml` exercises the transient burst-pool exhaustion path
- `repro-2seg-cpu.yaml` and `repro-1seg-cpu.yaml` are CPU-only variants
- regulating the burst rate reduces backpressure, but is not the fix

## YAML configs

Three YAML configs isolate which variables contribute to the burst ceiling:

| Config | `num_segs` | `use_gpu` | Memory regions | Purpose |
|--------|-----------|-----------|----------------|---------|
| `repro.yaml` | 2 | true | CPU header + GPU payload | Baseline (reproduces ceiling) |
| `repro-2seg-cpu.yaml` | 2 | false | CPU header + CPU payload | Isolates GPU vs multi-segment |
| `repro-1seg-cpu.yaml` | 1 | false | Single CPU region | Isolates multi-segment chaining |

**Knobs:**

| Field | Default | Description |
|-------|---------|-------------|
| `num_segs` | 2 | Segments per packet: 1 (single CPU) or 2 (header + payload) |
| `use_gpu` | true | GPU device memory for payload segment (must be false when `num_segs == 1`) |


## Build

Requires the ANO-capable container (DPDK + Holoscan SDK + ANO).

```bash
./holohub build adv_networking_repro_gpu_tx --language cpp
```

## Setup

Edit the YAML configs for your host before running:

| Field | What to change |
|-------|----------------|
| `address` | NIC PCIe address (`lspci \| grep Mellanox`) |
| `cpu_core` | Isolated core for the DPDK TX worker |
| `src_ip` / `dst_ip` | IP addresses for your link |
| `dst_mac` | Destination NIC MAC |

## CLI

```
Usage: adv_networking_repro_gpu_tx [--yaml <path>] [--pace <us>]
  --yaml   Config file (default: repro.yaml next to binary)
  --pace   Microseconds between bursts (default: 0 = tight loop)
```

## Run

Requires root for DPDK hugepage and NIC access.

```bash
# Baseline 2-seg GPU (may hit transient exhaustion and retry, should recover)
./holohub run adv_networking_repro_gpu_tx \
  --language cpp --as-root --docker-opts="--privileged"

# With pacing
./holohub run adv_networking_repro_gpu_tx \
  --language cpp --as-root --docker-opts="--privileged" \
  --run-args="--pace 50"

# 2-seg CPU (isolates GPU vs multi-segment)
./holohub run adv_networking_repro_gpu_tx \
  --language cpp --as-root --docker-opts="--privileged" \
  --run-args="--yaml repro-2seg-cpu.yaml"

# 1-seg CPU (isolates multi-segment chaining)
./holohub run adv_networking_repro_gpu_tx \
  --language cpp --as-root --docker-opts="--privileged" \
  --run-args="--yaml repro-1seg-cpu.yaml"

# Pacing sweep on any variant
for p in 0 10 50 100 250; do
  echo "=== pace=$p ==="
  ./holohub run adv_networking_repro_gpu_tx \
    --language cpp --as-root --docker-opts="--privileged" \
    --run-args="--pace $p" \
    2>&1 | grep -E 'pace_us|bursts_sent|packets|throughput|retri|exit_code'
done
```

## Expected results

- `repro.yaml --pace 0`: may log transient burst-pool exhaustion in
  `get_tx_packet_burst()`, then retry and complete without `SIGSEGV`
- `repro.yaml --pace 50`: should complete cleanly
- `repro-2seg-cpu.yaml --pace 0`: should complete cleanly
- `repro-1seg-cpu.yaml --pace 0`: should complete cleanly
