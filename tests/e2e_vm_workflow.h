#pragma once

#include "kv/kv_store.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tigonkv::e2e_vm_workflow {

inline std::string Env(const char *name, const std::string &fallback = {}) {
  const char *value = std::getenv(name);
  return value && *value ? value : fallback;
}

inline uint64_t PositiveEnv(const char *name, uint64_t fallback) {
  const std::string text = Env(name, std::to_string(fallback));
  size_t used = 0;
  const uint64_t value = std::stoull(text, &used);
  if (used != text.size() || value == 0) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  return value;
}

inline uint32_t NodeId() {
  const std::string text = Env("TIGONKV_NODE_ID", "0");
  size_t used = 0;
  const uint64_t value = std::stoull(text, &used);
  if (used != text.size() || value > UINT32_MAX)
    throw std::invalid_argument("TIGONKV_NODE_ID must be an unsigned integer");
  return static_cast<uint32_t>(value);
}

inline Config LoadConfig() {
  const std::string path = Env("TIGONKV_EXPERIMENT_CONFIG_JSONC", "experiment_config.jsonc");
  Config config = Config::FromJsonc(path);
  config.node_id = NodeId();
  if (config.node_id >= config.vm_count) {
    throw std::invalid_argument("TIGONKV_NODE_ID is outside vm.count");
  }
  return config;
}

inline uint64_t StartForPart(uint64_t total, uint64_t parts, uint64_t part) {
  const uint64_t base = total / parts;
  const uint64_t extra = total % parts;
  return base * part + (part < extra ? part : extra);
}

inline uint64_t CountForPart(uint64_t total, uint64_t parts, uint64_t part) {
  const uint64_t base = total / parts;
  return base + (part < total % parts ? 1 : 0);
}

inline uint64_t NowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline uint64_t SplitMix64(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

inline char Base36(uint64_t value) {
  return static_cast<char>(value < 10 ? '0' + value : 'A' + (value - 10));
}

inline std::string Key08(uint64_t index) {
  std::string key(8, '0');
  uint64_t value = index;
  for (size_t pos = key.size(); pos-- > 0;) {
    key[pos] = Base36(value % 36);
    value /= 36;
  }
  key[0] = 'k';
  return key;
}

inline std::string Key09(uint64_t index) {
  std::string key(32, '0');
  uint64_t value = index;
  for (size_t pos = key.size(); pos-- > 1;) {
    key[pos] = Base36(value % 36);
    value /= 36;
  }
  key[0] = 'u';
  return key;
}

inline std::string Value09(uint64_t index, uint64_t generation) {
  std::string value(1000, '!');
  uint64_t state = SplitMix64(index ^ (generation << 48U) ^ 0xc209ULL);
  for (size_t i = 0; i < value.size(); ++i) {
    if ((i & 7U) == 0) state = SplitMix64(state + i + generation);
    value[i] = static_cast<char>('!' + ((state >> ((i & 7U) * 8U)) % 94U));
  }
  return value;
}

struct PhaseResult {
  uint64_t duration_us = 0;
  uint64_t operations = 0;
};

template <typename Operation>
PhaseResult RunWorkers(KVStore &store, const Config &base, uint64_t total, Operation operation) {
  const uint64_t threads = PositiveEnv("TIGONKV_E2E_THREADS",
                                       base.foreground_worker_count_per_vm);
  const uint64_t node_count = CountForPart(total, base.vm_count, base.node_id);
  const uint64_t node_start = StartForPart(total, base.vm_count, base.node_id);

  // A node owns one KVStore/transport dispatcher.  Its inbound MPSC dequeue
  // is serialized inside KVEngine, while foreground operations remain
  // concurrent and retain independent worker ranges.

  std::atomic<bool> start{false};
  std::mutex error_mutex;
  std::exception_ptr error;
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(threads));
  const uint64_t phase_start = NowUs();
  for (uint64_t worker = 0; worker < threads; ++worker) {
    workers.emplace_back([&, worker] {
      try {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        const uint64_t worker_start = StartForPart(node_count, threads, worker);
        const uint64_t worker_count = CountForPart(node_count, threads, worker);
        for (uint64_t i = 0; i < worker_count; ++i)
          operation(store, worker, node_start + worker_start + i, i);
      } catch (...) {
        std::lock_guard<std::mutex> guard(error_mutex);
        if (!error) error = std::current_exception();
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &worker : workers) worker.join();
  const uint64_t phase_end = NowUs();
  if (error) std::rethrow_exception(error);
  return {phase_end - phase_start, node_count};
}

inline void CheckpointOrThrow(KVStore &store) {
  const Status status = store.Checkpoint();
  if (!status.ok()) throw std::runtime_error("phase checkpoint failed: " + status.message);
}

inline void DrainTransport(KVStore &store) {
  const uint64_t drain_ms = PositiveEnv("TIGONKV_E2E_TRANSPORT_DRAIN_MS", 5000);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const Status status = store.PollTransport();
    if (!status.ok()) throw std::runtime_error("phase transport drain failed: " + status.message);
    std::this_thread::yield();
  }
}

inline int RunE2E08MultiVm() {
  Config config = LoadConfig();
  const bool init_only = Env("TIGONKV_E2E_MULTI_VM_INIT_ONLY") == "1";
  const bool reset = Env("TIGONKV_E2E_RESET") == "1";
  auto main_store = KVStore::Create(config, reset);
  if (init_only) {
    CheckpointOrThrow(*main_store);
    std::cout << "TIGONKV_E2E_MULTI_VM_INIT node=" << config.node_id << " passed.\n";
    return 0;
  }
  const std::string phase = Env("TIGONKV_E2E_PHASE", "fill");
  const uint64_t total = PositiveEnv("TIGONKV_E2E08_TOTAL_KEYS", 100000);
  PhaseResult result;
  if (phase == "fill") {
    result = RunWorkers(*main_store, config, total, [](KVStore &store, uint64_t, uint64_t index, uint64_t) {
      const std::string key = Key08(index);
      const Status status = store.Put(key, key);
      if (!status.ok()) throw std::runtime_error("e2e08 fill PUT failed: " + status.message);
    });
  } else if (phase == "read") {
    result = RunWorkers(*main_store, config, total, [&](KVStore &store, uint64_t worker, uint64_t, uint64_t i) {
      const uint64_t index = SplitMix64(0xe2080000ULL ^
          (static_cast<uint64_t>(config.node_id) << 32U) ^
          ((worker + 1) << 16U) ^ i) % total;
      const std::string key = Key08(index);
      const GetResult got = store.Get(key);
      if (!got.status.ok() || got.value != key)
        throw std::runtime_error("e2e08 read verification failed for index " + std::to_string(index) +
                                 " status=" + std::to_string(static_cast<uint32_t>(got.status.code)) +
                                 " message=" + got.status.message +
                                 " value_size=" + std::to_string(got.value.size()));
    });
  } else {
    throw std::invalid_argument("e2e08 phase must be fill or read");
  }
  DrainTransport(*main_store);
  std::cout << "E2E_08_PHASE_TIME_US node=" << config.node_id << " phase=" << phase
            << " duration_us=" << result.duration_us << " op_count=" << result.operations << "\n";
  std::cout << "E2E_08_THREADS node=" << config.node_id << " threads="
            << PositiveEnv("TIGONKV_E2E_THREADS", config.foreground_worker_count_per_vm) << "\n";
  CheckpointOrThrow(*main_store);
  std::cout << main_store->DumpStats();
  std::cout << "e2e_08_vm[node" << config.node_id << "]: passed.\n";
  return 0;
}

inline int RunE2E09MultiVm() {
  Config config = LoadConfig();
  const bool init_only = Env("TIGONKV_E2E_MULTI_VM_INIT_ONLY") == "1";
  const bool reset = Env("TIGONKV_E2E_RESET") == "1";
  auto main_store = KVStore::Create(config, reset);
  if (init_only) {
    CheckpointOrThrow(*main_store);
    std::cout << "TIGONKV_E2E_MULTI_VM_INIT node=" << config.node_id << " passed.\n";
    return 0;
  }
  const std::string phase = Env("TIGONKV_E2E_PHASE", "fill");
  const uint64_t total = PositiveEnv("TIGONKV_E2E09_TOTAL_KEYS", 100000);
  PhaseResult result;
  if (phase == "fill" || phase == "update") {
    const uint64_t generation = phase == "update" ? 1 : 0;
    result = RunWorkers(*main_store, config, total, [generation](KVStore &store, uint64_t, uint64_t index, uint64_t) {
      const Status status = store.Put(Key09(index), Value09(index, generation));
      if (!status.ok()) throw std::runtime_error("e2e09 PUT failed: " + status.message);
    });
  } else if (phase == "read") {
    result = RunWorkers(*main_store, config, total, [&](KVStore &store, uint64_t worker, uint64_t, uint64_t i) {
      const uint64_t index = SplitMix64(0xe2090000ULL ^
          (static_cast<uint64_t>(config.node_id) << 32U) ^
          ((worker + 1) << 16U) ^ i) % total;
      const GetResult got = store.Get(Key09(index));
      if (!got.status.ok() || got.value != Value09(index, 1))
        throw std::runtime_error("e2e09 read verification failed for index " + std::to_string(index));
    });
  } else {
    throw std::invalid_argument("e2e09 phase must be fill, update, or read");
  }
  DrainTransport(*main_store);
  std::cout << "E2E_09_PHASE_TIME_US node=" << config.node_id << " phase=" << phase
            << " duration_us=" << result.duration_us << " op_count=" << result.operations << "\n";
  std::cout << "E2E_09_THREADS node=" << config.node_id << " threads="
            << PositiveEnv("TIGONKV_E2E_THREADS", config.foreground_worker_count_per_vm) << "\n";
  CheckpointOrThrow(*main_store);
  std::cout << main_store->DumpStats();
  std::cout << "e2e_09_vm[node" << config.node_id << "]: passed.\n";
  return 0;
}

}  // namespace tigonkv::e2e_vm_workflow
