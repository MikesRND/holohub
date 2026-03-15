# Findings

This note captures the current observed behavior from the demo app after the
DPDK TX hardening and partial-cleanup fixes.

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

- `2-seg GPU` at `pace_us=0` no longer segfaults; it hits transient burst-pool
  exhaustion, retries, and completes cleanly
- `2-seg GPU` succeeds cleanly at `pace_us=50`
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
| 2-seg GPU | 0 | 200/200 | 27 | clean after transient burst-pool exhaustion and retry | 400 |
| 2-seg GPU | 50 | 200/200 | 0 | clean | 400 |
| 2-seg GPU | 100 | 200/200 | 0 | clean | 400 |
| 2-seg CPU | 0 | 200/200 | 0 | clean | 400 |
| 1-seg CPU | 0 | 200/200 | 0 | clean | 400 |

Observations:

- Both CPU control variants complete at full speed with zero retries.
- The only configuration that needs retries is the one that combines
  `num_segs=2` with GPU-backed payload memory at `pace_us=0`.
- In the latest run, transient exhaustion occurred at `seg=1` around burst 63,
  then recovered after 27 retries.
- Pacing removes the retry pressure in the tested GPU-backed runs.

## Historical crash signature

Before the cleanup fix, failing GPU runs produced:

```text
Signal 11 (Segmentation fault: address not mapped to object at address (nil))
  librte_mempool_ring.so.25.0(+0x1b6c)
  libholoscan_advanced_network_dpdk.so(DpdkMgr::get_tx_packet_burst+0x71c)
  adv_networking_repro_gpu_tx(main)
```

The current fixed behavior no longer reproduces this crash.
