# Advanced Networking GPU TX Reproducer

Minimal reproducer for the ANO split-TX ~63 burst ceiling.

Single-file C++ (`main.cpp`), raw ANO API calls, no application framework.
Retries on `NO_FREE_BURST_BUFFERS` as backpressure and reports stats.

## Problem

With 2-segment (HDS) TX — CPU header pool + GPU payload pool —
`get_tx_packet_burst()` returns `NO_FREE_BURST_BUFFERS` after ~63
consecutive sends, regardless of pool sizes. The burst buffers are not
being reclaimed fast enough; adding inter-burst pacing (`--pace >= 50`)
lets the TX worker catch up and the ceiling disappears.

Holoscan's `adv_networking_bench` succeeds on the same link, likely
because its Holoscan scheduler introduces enough pacing naturally.

## Probe variants

Three YAML configs isolate which variables contribute to the burst ceiling:

| Config | `num_segs` | `use_gpu` | Memory regions | Purpose |
|--------|-----------|-----------|----------------|---------|
| `repro.yaml` | 2 | true | CPU header + GPU payload | Baseline (reproduces ceiling) |
| `repro-2seg-cpu.yaml` | 2 | false | CPU header + CPU payload | Isolates GPU vs multi-segment |
| `repro-1seg-cpu.yaml` | 1 | false | Single CPU region | Isolates multi-segment chaining |

## Confirmed results

Tested in `rnd-containers/ano-dev` with 2 packets per burst and 2 TX
segments per packet.

### Pacing sweep

| pace_us | bursts_sent | retries | result |
|---------|-------------|---------|--------|
| 0       | 200         | heavy   | backpressure retries kick in around burst 63 |
| 10      | 200         | heavy   | same |
| 50      | 200         | 0       | no backpressure |
| 100     | 200         | 0       | no backpressure |
| 250     | 200         | 0       | no backpressure |

Compare `packets_sent` against DPDK `tx_good_packets` (printed on
shutdown) to detect TX-side loss.

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

### YAML knobs

| Field | Default | Description |
|-------|---------|-------------|
| `num_segs` | 2 | Segments per packet: 1 (single CPU) or 2 (header + payload) |
| `use_gpu` | true | GPU device memory for payload segment (must be false when `num_segs == 1`) |

## Run

Requires root for DPDK hugepage and NIC access.

```bash
# Baseline 2-seg GPU (reproduces burst ceiling)
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

## Recommended data collection

Collect this minimal set first:

```bash
# Baseline 2-seg GPU
./holohub run adv_networking_repro_gpu_tx \
  --language cpp --as-root --docker-opts="--privileged" \
  --run-args="--pace 0"

# 2-seg CPU
./holohub run adv_networking_repro_gpu_tx \
  --language cpp --as-root --docker-opts="--privileged" \
  --run-args="--yaml repro-2seg-cpu.yaml --pace 0"

# 1-seg CPU
./holohub run adv_networking_repro_gpu_tx \
  --language cpp --as-root --docker-opts="--privileged" \
  --run-args="--yaml repro-1seg-cpu.yaml --pace 0"

# Optional pacing comparison on baseline
./holohub run adv_networking_repro_gpu_tx \
  --language cpp --as-root --docker-opts="--privileged" \
  --run-args="--pace 50"
```

Compare these summary fields across runs:

- `bursts_sent`
- `bursts_retried`
- `total_retries`
- `max_retries_burst`
- `first_failure_stage`
- `first_failure_status`
- `exit_code`

## Files

| File | Purpose |
|------|---------|
| `main.cpp` | Entire reproducer |
| `repro.yaml` | Baseline config — 2-seg GPU (edit host-specific values) |
| `repro-2seg-cpu.yaml` | Probe — 2-seg CPU (both segments hugepage) |
| `repro-1seg-cpu.yaml` | Probe — 1-seg CPU (single contiguous segment) |
| `CMakeLists.txt` | Build config — links ANO, CUDA, yaml-cpp |
