# Findings

This note captures observed behavior from the demo app. 

## Test setup

Observed on:

- x86_64 multi-core host
- ConnectX-class NIC
- Holoscan 3.11.0 environment
- DPDK 24.11.3
- mlx5 net PMD
- Reproducer config: 2 packets per burst, `batch_size: 2`, `tx_meta_buffers: 256`

## Observed behavior

Observed across current runs:

- `2-seg GPU` crashes in `DpdkMgr::get_tx_packet_burst()` at `pace_us=0`
- `2-seg GPU` also crashes in the same place at `pace_us=50`
- `2-seg GPU` succeeds cleanly at `pace_us=100`
- `2-seg CPU` succeeds cleanly at `pace_us=0`
- `1-seg CPU` succeeds cleanly at `pace_us=0`

## Conditions

The reproducer variants isolate whether the failure tracks GPU-backed memory, multi-segment chaining, or both:

| Variant | `num_segs` | `use_gpu` | Memory regions |
|---------|-----------|-----------|----------------|
| 2-seg GPU | 2 | true | `TX_Hdr` (huge) + `TX_Payload` (device) |
| 2-seg CPU | 2 | false | `TX_Hdr` (huge) + `TX_Payload` (huge) |
| 1-seg CPU | 1 | false | `TX_Data` (huge) |

## Results

### Comparison

| Variant | `pace_us` | bursts_sent | retries | result | DPDK `tx_good_packets` |
|---------|-----------|-------------|---------|--------|------------------------|
| 2-seg GPU | 0 | 63 | 0 | SIGSEGV in `get_tx_packet_burst()` | none (no clean shutdown) |
| 2-seg GPU | 50 | 63 | 0 | SIGSEGV in `get_tx_packet_burst()` | none (no clean shutdown) |
| 2-seg GPU | 100 | 200/200 | 0 | clean | 400 |
| 2-seg GPU | 100 | 200/200 | 0 | clean (repeat) | 400 |
| 2-seg CPU | 0 | 200/200 | 0 | clean | 400 |
| 1-seg CPU | 0 | 200/200 | 0 | clean | 400 |

Observations:

- Both CPU control variants complete at full speed with zero retries.
- The failing configuration is the one that combines `num_segs=2` with GPU-backed payload memory.
- Pacing changes the outcome for the GPU-backed variant, but only at the higher tested value (`100us`).

## Crash signature

Observed crash across failing GPU runs:

```text
Signal 11 (Segmentation fault: address not mapped to object at address (nil))
  librte_mempool_ring.so.25.0(+0x1b6c)
  libholoscan_advanced_network_dpdk.so(DpdkMgr::get_tx_packet_burst+0x71c)
  adv_networking_repro_gpu_tx(main)
```

Observations:

- The crash happens during burst acquisition, not during payload fill or send.
- The crash is inside `DpdkMgr::get_tx_packet_burst()`, with the top external
  frame in `librte_mempool_ring.so.25.0`.
- In this dataset, the crash occurs after 63 successful sends in the failing GPU-backed runs.

