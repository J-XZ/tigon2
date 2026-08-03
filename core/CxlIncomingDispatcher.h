#pragma once

#include "common/BufferedReader.h"
#include "kv/engine/mem_access.h"

#include <atomic>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace star {

// The CXL-only receive body extracted from IncomingDispatcher.  The caller
// supplies the existing worker-queue binding; this function owns no queue,
// message, retry, or batching state.
template <typename Enqueue>
void RunCxlIncomingLoop(MPSCRingBuffer &ring, uint32_t coord_id,
                        uint32_t worker_count, std::atomic<bool> &stop,
                        Enqueue &&enqueue) {
  BufferedReader reader(ring);
  while (!stop.load(std::memory_order_acquire)) {
    bool empty_poll = false;
    {
      // One non-nested receive scope per iteration: dequeue attempt plus a
      // successful worker handoff. Empty polls settle before yield.
      tigonkv::engine::mem_access::LatencyScope receive_scope(
          latency_sim::ScopeKind::kOther);
      std::unique_ptr<Message> message;
      try {
        message = reader.next_message();
      } catch (const std::exception &error) {
        std::ostringstream detail;
        detail << "CXL receive coord=" << coord_id << " ring=" << &ring
               << ": " << error.what();
        throw std::runtime_error(detail.str());
      }
      if (message == nullptr) {
        empty_poll = true;
      } else {
        if (message->get_dest_node_id() != coord_id)
          throw std::runtime_error("CXL receive destination node mismatch");
        if (message->get_worker_id() >= worker_count)
          throw std::runtime_error("CXL receive worker id exceeds worker count");

        // Enqueue receives the unique owner and is responsible for releasing it
        // only after the existing queue handoff succeeds.
        const uint32_t worker_id = message->get_worker_id();
        enqueue(worker_id, std::move(message));
      }
    }
    if (empty_poll) std::this_thread::yield();
  }
}

}  // namespace star
