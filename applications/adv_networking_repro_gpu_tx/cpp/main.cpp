/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Minimal ANO split-TX burst ceiling reproducer.
// Single file, raw ANO API calls.
//
// Demonstrates: get_tx_packet_burst() returns NO_FREE_BURST_BUFFERS after ~63
// consecutive sends with 2-segment TX, regardless of pool sizes. Retries on
// NO_FREE_BURST_BUFFERS as backpressure. pace_us controls inter-burst delay —
// values >= 50 let the TX worker reclaim; 0 shows heavy retries.

#include <advanced_network/common.h>
#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace ano = holoscan::advanced_network;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

static constexpr const char* status_string(ano::Status s) noexcept {
  switch (s) {
    case ano::Status::SUCCESS:                return "SUCCESS";
    case ano::Status::NULL_PTR:               return "NULL_PTR";
    case ano::Status::NO_FREE_BURST_BUFFERS:  return "NO_FREE_BURST_BUFFERS";
    case ano::Status::NO_FREE_PACKET_BUFFERS: return "NO_FREE_PACKET_BUFFERS";
    case ano::Status::NOT_READY:              return "NOT_READY";
    case ano::Status::INVALID_PARAMETER:      return "INVALID_PARAMETER";
    case ano::Status::NO_SPACE_AVAILABLE:     return "NO_SPACE_AVAILABLE";
    case ano::Status::NOT_SUPPORTED:          return "NOT_SUPPORTED";
    case ano::Status::GENERIC_FAILURE:        return "GENERIC_FAILURE";
    case ano::Status::CONNECT_FAILURE:        return "CONNECT_FAILURE";
    case ano::Status::INTERNAL_ERROR:         return "INTERNAL_ERROR";
  }
  return "UNKNOWN";
}

static std::pair<bool, unsigned int> parse_ipv4(const char* s) {
  std::array<unsigned int, 4> o{};
  if (std::sscanf(s, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) != 4)
    return {false, 0};
  for (auto v : o)
    if (v > 255) return {false, 0};
  return {true, (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3]};
}

// Fixed protocol constants — always Eth+IPv4+UDP.
constexpr int kWireHeaderSize = 42;   // Ethernet(14) + IPv4(20) + UDP(8)
constexpr int kUdpHdrSize = 8;
constexpr uint8_t kUdpProto = 17;
constexpr int kMaxRetries = 100000;   // ~1s at 10us per retry

int main(int argc, char** argv) {
  // --- CLI ---
  std::string yaml_path;
  int pace_us = 0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::fprintf(stderr,
          "Usage: %s [--yaml <path>] [--pace <us>]\n"
          "Minimal ANO split-TX burst ceiling reproducer.\n"
          "  --yaml   Config file (default: repro.yaml next to binary)\n"
          "  --pace   Microseconds between bursts (default: 0 = tight loop)\n",
          argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--yaml") == 0) {
      if (i + 1 >= argc) { std::fprintf(stderr, "ERROR: --yaml requires a path\n"); return 1; }
      yaml_path = argv[++i];
    } else if (std::strcmp(argv[i], "--pace") == 0) {
      if (i + 1 >= argc) { std::fprintf(stderr, "ERROR: --pace requires a value\n"); return 1; }
      char* end = nullptr;
      pace_us = static_cast<int>(std::strtol(argv[++i], &end, 10));
      if (end == argv[i] || *end != '\0') {
        std::fprintf(stderr, "ERROR: --pace: not an integer: %s\n", argv[i]);
        return 1;
      }
    }
  }

  if (pace_us < 0) { std::fprintf(stderr, "ERROR: --pace %d < 0\n", pace_us); return 1; }

  if (yaml_path.empty())
    yaml_path = (std::filesystem::path(argv[0]).parent_path() / "repro.yaml").string();

  // If --yaml was given with a relative path, resolve against the binary directory.
  // This ensures "./holohub run" works when CWD differs from the binary location.
  if (!yaml_path.empty() && std::filesystem::path(yaml_path).is_relative() &&
      !std::filesystem::exists(yaml_path)) {
    auto bin_dir = std::filesystem::canonical(argv[0]).parent_path();
    auto candidate = bin_dir / yaml_path;
    if (std::filesystem::exists(candidate))
      yaml_path = candidate.string();
  }

  // --- Load YAML ---
  YAML::Node root;
  try { root = YAML::LoadFile(yaml_path); }
  catch (const std::exception& e) {
    std::fprintf(stderr, "ERROR: %s: %s\n", yaml_path.c_str(), e.what());
    return 1;
  }

  auto repro = root["repro"];
  if (!repro.IsDefined()) { std::fprintf(stderr, "ERROR: missing repro: section\n"); return 1; }

  // --- Repro knobs ---
  int  gpu_device    = repro["gpu_device"].as<int>(0);
  int  port_id       = repro["port_id"].as<int>(0);
  int  queue_id      = repro["queue_id"].as<int>(0);
  int  num_packets   = repro["num_packets"].as<int>(2);
  int  max_bursts    = repro["max_bursts"].as<int>(200);
  int  header_bytes  = repro["header_bytes"].as<int>(64);
  int  payload_bytes = repro["payload_bytes"].as<int>(64);
  auto src_ip_str    = repro["src_ip"].as<std::string>();
  auto dst_ip_str    = repro["dst_ip"].as<std::string>();
  auto src_port      = repro["src_port"].as<uint16_t>(4096);
  auto dst_port      = repro["dst_port"].as<uint16_t>(4096);
  auto dst_mac       = repro["dst_mac"].as<std::string>();
  int  num_segs      = repro["num_segs"].as<int>(2);
  bool use_gpu       = repro["use_gpu"].as<bool>(true);

  if (num_segs < 1 || num_segs > 2) {
    std::fprintf(stderr, "ERROR: num_segs must be 1 or 2 (got %d)\n", num_segs);
    return 1;
  }
  if (num_segs == 1 && use_gpu) {
    std::fprintf(stderr, "ERROR: use_gpu must be false when num_segs == 1\n");
    return 1;
  }
  if (header_bytes < kWireHeaderSize) {
    std::fprintf(stderr, "ERROR: header_bytes %d < wire header size %d\n",
                 header_bytes, kWireHeaderSize);
    return 1;
  }

  auto [src_ok, src_ip] = parse_ipv4(src_ip_str.c_str());
  auto [dst_ok, dst_ip] = parse_ipv4(dst_ip_str.c_str());
  if (!src_ok) { std::fprintf(stderr, "ERROR: bad src_ip: %s\n", src_ip_str.c_str()); return 1; }
  if (!dst_ok) { std::fprintf(stderr, "ERROR: bad dst_ip: %s\n", dst_ip_str.c_str()); return 1; }

  // --- ANO config ---
  auto ano_node = root["advanced_network"]["cfg"];
  if (!ano_node.IsDefined()) { std::fprintf(stderr, "ERROR: missing advanced_network.cfg\n"); return 1; }

  ano::NetworkConfig net_config;
  try { net_config = ano_node.as<ano::NetworkConfig>(); }
  catch (const std::exception& e) {
    std::fprintf(stderr, "ERROR: ANO config decode: %s\n", e.what());
    return 1;
  }

  // --- Print config ---
  std::printf("=== ANO TX DEBUG REPRO ===\n");
  std::printf("config:         %s\n", yaml_path.c_str());
  std::printf("gpu_device:     %d\n", gpu_device);
  std::printf("port/queue:     %d/%d\n", port_id, queue_id);
  std::printf("num_packets:    %d  num_segs: %d\n", num_packets, num_segs);
  std::printf("use_gpu:        %s\n", use_gpu ? "true" : "false");
  std::printf("max_bursts:     %d\n", max_bursts);
  std::printf("payload_bytes:  %d  header_bytes: %d  wire_header: %d\n",
              payload_bytes, header_bytes, kWireHeaderSize);
  std::printf("src:            %s:%u\n", src_ip_str.c_str(), src_port);
  std::printf("dst:            %s:%u  mac: %s\n", dst_ip_str.c_str(), dst_port, dst_mac.c_str());
  std::printf("pace_us:        %d\n", pace_us);
  std::printf("\n");

  std::printf("=== ANO CONFIG ===\n");
  { YAML::Emitter e; e << ano_node; std::printf("%s\n\n", e.c_str()); }

  // --- Init CUDA + ANO ---
  if (use_gpu) {
    auto cerr = cudaSetDevice(gpu_device);
    if (cerr != cudaSuccess) {
      std::fprintf(stderr, "ERROR: cudaSetDevice(%d): %s\n", gpu_device, cudaGetErrorString(cerr));
      return 1;
    }
  }

  std::printf("Initializing ANO...\n");
  auto is = ano::adv_net_init(net_config);
  if (is != ano::Status::SUCCESS) {
    std::fprintf(stderr, "ERROR: adv_net_init: %s\n", status_string(is));
    return 1;
  }
  std::printf("ANO initialized.\n\n");

  // Convert ASCII MAC to raw 6-byte form for set_eth_header.
  // set_eth_header memcpys the first 6 bytes directly into the Ethernet header,
  // so passing the ASCII string would produce garbage.
  char eth_dst[6];
  ano::format_eth_addr(eth_dst, dst_mac);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // --- TX loop ---
  int bursts_sent = 0;
  int total_retries = 0, max_retries_burst = 0, bursts_retried = 0;
  const char* fail_stage = nullptr;
  const char* fail_status = nullptr;
  bool done = false;

  std::printf("=== TX LOOP ===\n");
  auto t_start = std::chrono::steady_clock::now();

  for (int b = 0; b < max_bursts && !g_stop.load(std::memory_order_relaxed) && !done; ++b) {
    ano::BurstParams* burst = nullptr;
    int retries = 0;
    bool acquired = false;

    // Acquire — retry on backpressure (NO_FREE_BURST_BUFFERS)
    while (!acquired && !g_stop.load(std::memory_order_relaxed)) {
      burst = ano::create_tx_burst_params();
      if (!burst) {
        std::printf("[%3d] FAIL create_tx_burst_params: nullptr\n", b);
        fail_stage = "create_tx_burst_params"; fail_status = "nullptr";
        done = true; break;
      }

      ano::set_header(burst, static_cast<uint16_t>(port_id),
                      static_cast<uint16_t>(queue_id), num_packets, num_segs);

      if (!ano::is_tx_burst_available(burst)) {
        ano::free_tx_metadata(burst);
        if (retries < kMaxRetries) { ++retries; usleep(10); continue; }
        std::printf("[%3d] FAIL is_tx_burst_available after %d retries\n", b, retries);
        fail_stage = "is_tx_burst_available"; fail_status = "retry_exhausted";
        done = true; break;
      }

      auto as = ano::get_tx_packet_burst(burst);
      if (as == ano::Status::SUCCESS) { acquired = true; break; }

      ano::free_all_packets_and_burst_tx(burst);
      if (as == ano::Status::NO_FREE_BURST_BUFFERS && retries < kMaxRetries) {
        ++retries; usleep(10); continue;
      }
      std::printf("[%3d] FAIL get_tx_packet_burst: %s\n", b, status_string(as));
      fail_stage = "get_tx_packet_burst"; fail_status = status_string(as);
      done = true; break;
    }

    if (!acquired) break;

    total_retries += retries;
    if (retries > max_retries_burst) max_retries_burst = retries;
    if (retries > 0) ++bursts_retried;

    // Fill packet segments
    for (int i = 0; i < num_packets; ++i) {
      auto* seg0 = ano::get_segment_packet_ptr(burst, 0, i);

      if (num_segs == 1) {
        // Single CPU segment: header + payload contiguous
        std::memset(seg0, 0, static_cast<size_t>(header_bytes + payload_bytes));

        ano::set_eth_header(burst, i, eth_dst);
        ano::set_ipv4_header(burst, i, kUdpHdrSize + payload_bytes, kUdpProto, src_ip, dst_ip);
        ano::set_udp_header(burst, i, payload_bytes, src_port, dst_port);

        // Payload pattern after headers
        std::memset(static_cast<char*>(seg0) + header_bytes,
                    static_cast<uint8_t>(b & 0xFF),
                    static_cast<size_t>(payload_bytes));

        ano::set_packet_lengths(burst, i, {header_bytes + payload_bytes});
      } else {
        // Two segments: seg0 = header, seg1 = payload
        std::memset(seg0, 0, static_cast<size_t>(header_bytes));

        ano::set_eth_header(burst, i, eth_dst);
        ano::set_ipv4_header(burst, i, kUdpHdrSize + payload_bytes, kUdpProto, src_ip, dst_ip);
        ano::set_udp_header(burst, i, payload_bytes, src_port, dst_port);

        auto* seg1 = ano::get_segment_packet_ptr(burst, 1, i);
        if (use_gpu)
          cudaMemset(seg1, static_cast<uint8_t>(b & 0xFF), static_cast<size_t>(payload_bytes));
        else
          std::memset(seg1, static_cast<uint8_t>(b & 0xFF), static_cast<size_t>(payload_bytes));

        ano::set_packet_lengths(burst, i, {header_bytes, payload_bytes});
      }
    }

    if (use_gpu) cudaDeviceSynchronize();

    auto ss = ano::send_tx_burst(burst);
    if (ss != ano::Status::SUCCESS) {
      std::printf("[%3d] FAIL send_tx_burst: %s\n", b, status_string(ss));
      fail_stage = "send_tx_burst"; fail_status = status_string(ss);
      break;
    }

    ++bursts_sent;
    if (retries > 0)
      std::printf("[%3d] sent (%d retries)\n", b, retries);
    else
      std::printf("[%3d] sent\n", b);
    if (pace_us > 0) usleep(static_cast<useconds_t>(pace_us));
  }

  // --- Summary ---
  auto t_end = std::chrono::steady_clock::now();
  double elapsed_s = std::chrono::duration<double>(t_end - t_start).count();
  int actual_packets = bursts_sent * num_packets;
  int wire_bytes_per_pkt = header_bytes + payload_bytes;

  std::printf("\n=== SUMMARY ===\n");
  std::printf("config:               %s\n", yaml_path.c_str());
  std::printf("pace_us:              %d\n", pace_us);
  std::printf("elapsed_s:            %.3f\n", elapsed_s);
  std::printf("bursts_sent:          %d / %d\n", bursts_sent, max_bursts);
  std::printf("packets_sent:         %d  (expected %d)\n", actual_packets, max_bursts * num_packets);
  if (elapsed_s > 0) {
    double pkt_per_s = actual_packets / elapsed_s;
    double bits_per_s = actual_packets * wire_bytes_per_pkt * 8.0 / elapsed_s;
    std::printf("bursts/s:             %.0f\n", bursts_sent / elapsed_s);
    std::printf("packets/s:            %.0f\n", pkt_per_s);
    if (bits_per_s >= 1e9)
      std::printf("throughput:           %.3f Gbps\n", bits_per_s / 1e9);
    else if (bits_per_s >= 1e6)
      std::printf("throughput:           %.1f Mbps\n", bits_per_s / 1e6);
    else
      std::printf("throughput:           %.0f bps\n", bits_per_s);
  }
  std::printf("bursts_retried:       %d\n", bursts_retried);
  std::printf("total_retries:        %d\n", total_retries);
  std::printf("max_retries_burst:    %d\n", max_retries_burst);
  std::printf("first_failure_stage:  %s\n", fail_stage ? fail_stage : "none");
  std::printf("first_failure_status: %s\n", fail_stage ? fail_status : "none");

  int rc = fail_stage ? 1 : 0;
  std::printf("exit_code:            %d\n", rc);

  ano::shutdown();
  return rc;
}
