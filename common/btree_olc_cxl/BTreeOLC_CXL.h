#pragma once
// Adapted from https://github.com/zxjcarrot/spitfire/blob/main/include/engine/btreeolc.h
// Contributors: Jie Hou, Yilin Chen, Xinjing ZHou

// The shared-memory version of BTreeOLC, with RegionOffset links for
// position-independent persistence.
// and cxlalloc for dynamic memory management

#include <immintrin.h>
#include <sched.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>
#include <thread>
#include <random>
#include <type_traits> // std::{enable_if,is_trivial}

#include "common/CXLMemory.h"
#include "common/CXL_EBR.h"

#include "kv/engine/region_allocator.h"
#include "kv/engine/mem_access.h"


#include "glog/logging.h"

namespace btreeolc_cxl
{

extern thread_local uint32_t RWSpinLatchThreadId;

constexpr float kMergeThreshold = 0.4;
constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kLeafPageSize = 4096;
constexpr uint64_t kLockMask = 1 << 10;
constexpr uint64_t kReaderMask = kLockMask - 1;
constexpr uint64_t kVersionMask = ~kReaderMask;

// tigonkv: node placement is selected when a tree instance is constructed.
// It is intentionally a concrete direct binding (no virtual dispatch or
// callback) so a private tree can never silently fall back into HWCC.
struct TreeNodeAllocation {
	tigonkv::engine::DualRegionAllocator *regions = nullptr;
	tigonkv::engine::AllocationDomain domain = tigonkv::engine::AllocationDomain::kHwccIndex;
	uint32_t owner_shard = 0;
	star::CXL_EBR *ebr = nullptr;
	uint32_t private_partition = UINT32_MAX;

	void *Allocate(uint64_t bytes) const {
		if (regions == nullptr) throw std::invalid_argument("BPlusTree requires a region allocation binding");
		return domain == tigonkv::engine::AllocationDomain::kOwnerPrivateSwcc &&
		               private_partition != UINT32_MAX
		           ? regions->AllocateOwnerPrivate(bytes, private_partition, owner_shard)
		           : regions->Allocate(bytes, domain, owner_shard);
	}

	void Retire(void *pointer, uint64_t bytes) const {
		if (ebr == nullptr) throw std::runtime_error("BPlusTree requires an EBR binding for node retirement");
		ebr->add_retired_object(pointer, bytes, star::CXLMemory::INDEX_FREE, owner_shard,
		                        private_partition);
	}

	void *FromOffset(tigonkv::engine::RegionOffset offset) const {
		if (regions == nullptr)
			throw std::invalid_argument("BPlusTree requires a region allocation binding");
		if (offset == tigonkv::engine::kNullOffset) return nullptr;
		return domain == tigonkv::engine::AllocationDomain::kOwnerPrivateSwcc
		           ? regions->ResolveOwnerPrivate(offset, kPageSize,
		                                         private_partition, owner_shard)
		           : regions->ResolveDynamicHwcc(offset, kPageSize, owner_shard);
	}

	tigonkv::engine::RegionOffset ToOffset(void *pointer) const {
		if (regions == nullptr)
			throw std::invalid_argument("BPlusTree requires a region allocation binding");
		if (pointer == nullptr) return tigonkv::engine::kNullOffset;
		return domain == tigonkv::engine::AllocationDomain::kOwnerPrivateSwcc
		           ? regions->ToOwnerPrivateOffset(pointer, private_partition)
		           : regions->hwcc().ToOffset(pointer);
	}
};

inline thread_local bool TreeAccessIsHwcc = true;
inline thread_local const TreeNodeAllocation *TreeAccessBinding = nullptr;

template <typename T>
class PersistentOffset {
 public:
  PersistentOffset() = default;
  PersistentOffset(std::nullptr_t) {}
  PersistentOffset(T *pointer) { assign(pointer); }
  T *get() const {
    if (offset_ == tigonkv::engine::kNullOffset) return nullptr;
    if (TreeAccessBinding == nullptr)
      throw std::runtime_error("persistent tree offset resolved without binding");
    return static_cast<T *>(TreeAccessBinding->FromOffset(offset_));
  }
  T *operator->() const { return get(); }
  explicit operator bool() const { return offset_ != tigonkv::engine::kNullOffset; }
  operator T *() const { return get(); }
  PersistentOffset &operator=(std::nullptr_t) {
    offset_ = tigonkv::engine::kNullOffset;
    return *this;
  }
  PersistentOffset &operator=(T *pointer) {
    assign(pointer);
    return *this;
  }
  bool operator==(std::nullptr_t) const { return !static_cast<bool>(*this); }
  bool operator!=(std::nullptr_t) const { return static_cast<bool>(*this); }
  bool operator==(const PersistentOffset &other) const {
    return offset_ == other.offset_;
  }
  bool operator!=(const PersistentOffset &other) const {
    return offset_ != other.offset_;
  }

 private:
  void assign(T *pointer) {
    if (pointer == nullptr) {
      offset_ = tigonkv::engine::kNullOffset;
      return;
    }
    if (TreeAccessBinding == nullptr)
      throw std::runtime_error("persistent tree offset assigned without binding");
    offset_ = TreeAccessBinding->ToOffset(pointer);
  }
  tigonkv::engine::RegionOffset offset_{tigonkv::engine::kNullOffset};
};

static_assert(sizeof(PersistentOffset<void>) ==
                  sizeof(tigonkv::engine::RegionOffset),
              "persistent tree links must preserve the original offset slot width");

// A tree's allocation binding, not the last node visited by this thread,
// determines the memory domain. Nested private/shared operations restore the
// outer domain on return so an outer leaf unlock cannot be charged as HWCC.
class TreeAccessScope {
 public:
  explicit TreeAccessScope(const TreeNodeAllocation &allocation)
      : previous_(TreeAccessIsHwcc), previous_binding_(TreeAccessBinding) {
    TreeAccessIsHwcc =
        allocation.domain != tigonkv::engine::AllocationDomain::kOwnerPrivateSwcc;
    TreeAccessBinding = &allocation;
  }
  ~TreeAccessScope() {
    TreeAccessIsHwcc = previous_;
    TreeAccessBinding = previous_binding_;
  }

 private:
  bool previous_;
  const TreeNodeAllocation *previous_binding_;
};

inline void RecordTreeDataRead(const void *address, uint64_t bytes) {
	if (TreeAccessIsHwcc)
		tigonkv::engine::mem_access::HwccRead(address, bytes);
	else
		tigonkv::engine::mem_access::PrivateRead(address, bytes);
}

inline void RecordTreeDataWrite(const void *address, uint64_t bytes) {
	if (TreeAccessIsHwcc)
		tigonkv::engine::mem_access::HwccWrite(address, bytes);
	else
		tigonkv::engine::mem_access::PrivateWrite(address, bytes);
}

template <typename T>
inline T TreeAtomicLoad(const std::atomic<T> &value, std::memory_order order) {
	return TreeAccessIsHwcc
	           ? latency_sim::FixedLatencyAtomicLoad(
	                 value, order, latency_sim::AtomicDomain::kHwcc)
	           : latency_sim::FixedLatencyAtomicLoad(
	                 value, order, latency_sim::AtomicDomain::kOwnerPrivateSwcc);
}

template <typename T>
inline void TreeAtomicStore(std::atomic<T> &value, T desired,
                            std::memory_order order) {
	if (TreeAccessIsHwcc)
		latency_sim::FixedLatencyAtomicStore(
		    value, desired, order, latency_sim::AtomicDomain::kHwcc);
	else
		latency_sim::FixedLatencyAtomicStore(
		    value, desired, order,
		    latency_sim::AtomicDomain::kOwnerPrivateSwcc);
}

template <typename T>
inline bool TreeAtomicCompareExchangeStrong(
    std::atomic<T> &value, T &expected, T desired,
    std::memory_order success, std::memory_order failure) {
	return TreeAccessIsHwcc
	           ? latency_sim::FixedLatencyAtomicCompareExchangeStrong(
	                 value, expected, desired, success, failure,
	                 latency_sim::AtomicDomain::kHwcc)
	           : latency_sim::FixedLatencyAtomicCompareExchangeStrong(
	                 value, expected, desired, success, failure,
	                 latency_sim::AtomicDomain::kOwnerPrivateSwcc);
}

template <typename T>
inline bool TreeAtomicCompareExchangeWeak(
    std::atomic<T> &value, T &expected, T desired,
    std::memory_order success, std::memory_order failure) {
	return TreeAccessIsHwcc
	           ? latency_sim::FixedLatencyAtomicCompareExchangeWeak(
	                 value, expected, desired, success, failure,
	                 latency_sim::AtomicDomain::kHwcc)
	           : latency_sim::FixedLatencyAtomicCompareExchangeWeak(
	                 value, expected, desired, success, failure,
	                 latency_sim::AtomicDomain::kOwnerPrivateSwcc);
}

template <typename T>
inline T TreeAtomicFetchAdd(std::atomic<T> &value, T operand,
                            std::memory_order order) {
	return TreeAccessIsHwcc
	           ? latency_sim::FixedLatencyAtomicFetchAdd(
	                 value, operand, order, latency_sim::AtomicDomain::kHwcc)
	           : latency_sim::FixedLatencyAtomicFetchAdd(
	                 value, operand, order,
	                 latency_sim::AtomicDomain::kOwnerPrivateSwcc);
}

template <typename T>
inline T TreeAtomicFetchSub(std::atomic<T> &value, T operand,
                            std::memory_order order) {
	return TreeAccessIsHwcc
	           ? latency_sim::FixedLatencyAtomicFetchSub(
	                 value, operand, order, latency_sim::AtomicDomain::kHwcc)
	           : latency_sim::FixedLatencyAtomicFetchSub(
	                 value, operand, order,
                 latency_sim::AtomicDomain::kOwnerPrivateSwcc);
}

// Record the cache line containing the node latch and metadata at each node
// actually visited.  Charging an entire 4 KiB page per operation both
// over-counted untouched payload and missed non-root nodes.
inline void RecordTreeAccess(const TreeNodeAllocation &allocation, const void *page,
                             bool write) {
	if (page == nullptr) return;
	constexpr uint64_t kNodeMetadataBytes = 64;
	if (allocation.domain == tigonkv::engine::AllocationDomain::kOwnerPrivateSwcc) {
		if (write) tigonkv::engine::mem_access::PrivateWrite(page, kNodeMetadataBytes);
		else tigonkv::engine::mem_access::PrivateRead(page, kNodeMetadataBytes);
	} else {
		if (write) tigonkv::engine::mem_access::HwccWrite(page, kNodeMetadataBytes);
		else tigonkv::engine::mem_access::HwccRead(page, kNodeMetadataBytes);
	}
}

struct LatchBase {
	std::atomic<uint64_t> word{ 0 };

	uint64_t load(std::memory_order order = std::memory_order_seq_cst) {
		return TreeAtomicLoad(word, order);
	}
	void store(uint64_t value,
	           std::memory_order order = std::memory_order_seq_cst) {
		TreeAtomicStore(word, value, order);
	}
	bool compareExchangeStrong(uint64_t &expected, uint64_t desired) {
		return TreeAtomicCompareExchangeStrong(
		    word, expected, desired, std::memory_order_seq_cst,
		    std::memory_order_seq_cst);
	}
	bool compareExchangeWeak(uint64_t &expected, uint64_t desired,
	                         std::memory_order success,
	                         std::memory_order failure) {
		return TreeAtomicCompareExchangeWeak(word, expected, desired, success,
		                                     failure);
	}
	uint64_t fetchAdd(uint64_t value) {
		return TreeAtomicFetchAdd(word, value, std::memory_order_seq_cst);
	}
	uint64_t fetchSub(uint64_t value) {
		return TreeAtomicFetchSub(word, value, std::memory_order_seq_cst);
	}
};

class OLTPRWSpinLatch : public LatchBase {
    public:
	OLTPRWSpinLatch()
	{
		assert(RWSpinLatchThreadId > 0);
	}
	void acquireRead()
	{
		while (tryAcquireRead() == false) {
			_mm_pause();
		}
	}

	void acquireWrite()
	{
		while (tryAcquireWrite() == false) {
			_mm_pause();
		}
	}

	void releaseRead()
	{
		while (tryReleaseRead() == false) {
			_mm_pause();
		}
	}

	void releaseWrite()
	{
		while (tryReleaseWrite() == false) {
			_mm_pause();
		}
	}

	bool tryAcquireRead()
	{
		uint64_t w = load();
		uint64_t readerCount = getReaderCount(w);
		uint64_t writerId = getWriterId(w);
		if (writerId == 0) {
			uint64_t neww = makeNewWord(readerCount + 1, 0);
			return compareExchangeStrong(w, neww);
		}
		// Locked by other writer
		return false;
	}

	bool tryAcquireWrite()
	{
		uint64_t w = load();
		uint64_t readerCount = getReaderCount(w);
		uint64_t writerId = getWriterId(w);
		assert(RWSpinLatchThreadId > 0);
		if (writerId == 0 && readerCount == 0) {
			uint64_t neww = makeNewWord(readerCount, RWSpinLatchThreadId);
			return compareExchangeStrong(w, neww);
		}
		// Locked by other writer
		return false;
	}

    private:
	bool tryReleaseRead()
	{
		uint64_t w = load();
		uint64_t readerCount = getReaderCount(w);
		uint64_t writerId = getWriterId(w);
		assert(writerId == 0);
		assert(readerCount > 0);
		uint64_t neww = makeNewWord(readerCount - 1, 0);
		return compareExchangeStrong(w, neww);
	}

	bool tryReleaseWrite()
	{
		uint64_t w = load();
		uint64_t readerCount = getReaderCount(w);
		uint64_t writerId = getWriterId(w);
		assert(writerId == RWSpinLatchThreadId);
		assert(readerCount == 0);
		uint64_t neww = makeNewWord(0, 0);
		return compareExchangeStrong(w, neww);
	}

	inline uint64_t makeNewWord(uint32_t readerCount, uint32_t writerId)
	{
		uint64_t w = (((uint64_t)readerCount) << kReaderCountOffset) | (((uint64_t)writerId) << kWriterIdOffset);
		return w;
	}

	inline uint64_t getReaderCount(uint64_t w)
	{
		return (w >> kReaderCountOffset) & kReaderCountMask;
	}

	inline uint64_t getWriterId(uint64_t w)
	{
		return (w >> kWriterIdOffset) & kWriterIdMask;
	}

	static constexpr uint64_t kReaderCountOffset = 0;
	static constexpr uint64_t kReaderCountMask = 0xffffffff;
	static constexpr uint64_t kWriterIdOffset = 32;
	static constexpr uint64_t kWriterIdMask = 0xffffffff;
};

enum class RemovePredicateResult { GOOD = 0, VALUE_HAS_OTHER_REFERENCE, VALUE_NOT_SATISFYING_PREDICATE };
enum class RemoveResult { GOOD = 0, KEY_NOT_FOUND, VALUE_HAS_OTHER_REFERENCE, VALUE_NOT_SATISFYING_PREDICATE };

static inline void prefetch(char *ptr, size_t len)
{
	// if (ptr == nullptr) return;
	// for (char *p = ptr; p < ptr + len; p += 64) {
	//     __builtin_prefetch(p);
	// }
}

/**
 * class BPlusTree
 * This implementation assumes that KeyType has properly implemented default constructor, copy-constructor, assignment-constructor, destructor.
 * This class only manages the memory occupied by the KeyType object itself.
 * Therefore, KeyType should also properly implement deep-copying and destructor.
 * It will also use move-constructor if implemented.
 * @param KeyComparator a<b -1, a==b 0, a>b 1
 * @param ValueComparator a==b 0, a!=b 1
 */
template <class KeyType, class ValueType, class KeyComparator, class ValueComparator, std::size_t UpdateThreshold = 1024, uint64_t LeafPageSize = kLeafPageSize,
	  uint64_t InnerPageSize = kPageSize>
class BPlusTree {
    public:
	/** this is the element type of the leaf node */
	using KeyValuePair = std::pair<KeyType, ValueType>;

	/**
	 * enum class NodeType - B+ Tree node type
	 */
	enum class NodeType : uint8_t { BTreeInner = 1, BTreeLeaf = 2 };

	/**
	 * @brief NodeLock could either be a optimistc lock or a read-read spin lock.
	 * Its functionality is determined by the @RWLock parameter at construction.
	 */
	struct NodeLock : public LatchBase {
		// bool isRWLock = false;
	    public:
		NodeLock(bool isRWLock = false)
		{
			// this->isRWLock = isRWLock;
			// if (isRWLock) {
			//     word = 0;              // RW Spin Lock Initialzation
			// } else {
			store(kLockMask * 2); // Optimistic Lock Initialzation
			//}
		}

		bool isLocked(uint64_t version)
		{
			// assert(!isRWLock);
			return (version & kLockMask) != 0;
		}

		/**
		 * acquire success and increase counter
		 */
		uint64_t readLockOrRestart(bool &needRestart)
		{
			// if (isRWLock) {
			//     assert(getReaderCount(word.load()) < 20);
			//     acquireRead();
			//     assert(getReaderCount(word.load()) < 20);
			//     needRestart = false;
			//     //OLTP_LOG_INFO(RWSpinLatchThreadId," readLockOrRestart on ", (uint64_t)this,  ", Readers: ",getReaderCount(word.load()));
			//     return 0;
			// } else {
			uint64_t version;
			version = load(std::memory_order_relaxed);
			if (isLocked(version)) {
				// acquire read lock fail
				needRestart = true;
				_mm_pause();
			}
			return version;
			//}
		}

		void iteratorEnter(bool &needRestart)
		{
			uint64_t version;
			version = load(std::memory_order_relaxed);
			if (isLocked(version)) {
				needRestart = true;
				_mm_pause();
				return;
			}
			fetchAdd(1);
		}

		void iteratorLeave()
		{
			// if (isRWLock) {
			//     assert(getReaderCount(word.load()) < 20);
			//     releaseRead();
			//     assert(getReaderCount(word.load()) < 20);
			// } else {
			fetchSub(1);
			//}
		}

		void writeLockOrRestart(bool &needRestart)
		{
			// if (isRWLock) {
			//     assert(getReaderCount(word.load()) < 20);
			//     acquireWrite();
			//     assert(getReaderCount(word.load()) < 20);
			// } else {
			uint64_t version;
			version = readLockOrRestart(needRestart);
			if (needRestart)
				return;

			upgradeToWriteLockOrRestart(version, needRestart);
			if (needRestart)
				return;
			//}
		}

		void upgradeToWriteLockOrRestart(uint64_t &version, bool &needRestart)
		{
			// if (isRWLock) {
			//     assert(getReaderCount(word.load()) < 20);
			//     needRestart = RWLockUpgradeToWriteLock() == false;
			//     assert(getReaderCount(word.load()) < 20);
			//     //OLTP_LOG_INFO(RWSpinLatchThreadId, " upgradeToWriteLockOrRestart on ", (uint64_t)this,  ", Readers:
			//     ",getReaderCount(word.load()));
			// } else {
			if ((word & kReaderMask) != 0) {
				needRestart = true;
				return;
			}
			if (compareExchangeStrong(version, version + kLockMask)) {
				version = version + kLockMask;
			} else {
				//_mm_pause();
				needRestart = true;
			}
			//}
		}

		void downgradeToReadLock(uint64_t &version)
		{
			// if (isRWLock) {
			//     assert(getReaderCount(word.load()) < 20);
			//     RWLockDowngradeToReadLock();
			//     assert(getReaderCount(word.load()) < 20);
			// } else {
			version = fetchAdd(kLockMask);
			version += kLockMask;
			//}
		}

		void writeUnlock()
		{
			// if (isRWLock) {
			//     assert(getReaderCount(word.load()) < 20);
			//     //OLTP_LOG_INFO(RWSpinLatchThreadId, " writeUnlock on ", (uint64_t)this,  ", Readers: ",getReaderCount(word.load()));
			//     releaseWrite();
			//     assert(getReaderCount(word.load()) < 20);
			// } else {
			fetchAdd(kLockMask);
			//}
		}

		void checkOrRestart(uint64_t startRead, bool &needRestart)
		{
			// if (isRWLock) {
			//     return;
			// }
			readUnlockOrRestart(startRead, needRestart, false);
		}

		void readUnlockOrRestart(uint64_t startRead, bool &needRestart, bool ebr = true)
		{
			// if (isRWLock) {
			//     assert(getReaderCount(word.load()) < 20);
			//     //OLTP_LOG_INFO(RWSpinLatchThreadId," readUnlockOrRestart on ", (uint64_t)this,  ", Readers: ",getReaderCount(word.load()));
			//     releaseRead();
			//     needRestart = false;
			// } else {
			needRestart = (((startRead ^ load()) & kVersionMask) != 0);
			//}
		}

		void acquireRead()
		{
			while (tryAcquireRead() == false) {
				_mm_pause();
			}
		}

		void acquireWrite()
		{
			while (tryAcqurieWrite() == false) {
				_mm_pause();
			}
		}

		void releaseRead()
		{
			while (tryReleaseRead() == false) {
				_mm_pause();
			}
		}

		void releaseWrite()
		{
			while (tryReleaseWrite() == false) {
				_mm_pause();
			}
		}

		bool tryAcquireRead()
		{
			uint64_t w = load();
			uint64_t readerCount = getReaderCount(w);
			uint64_t writerId = getWriterId(w);
			if (writerId == 0) {
				uint64_t neww = makeNewWord(readerCount + 1, 0);
				return compareExchangeStrong(w, neww);
			}
			// Locked by other writer
			return false;
		}

		bool RWLockUpgradeToWriteLock()
		{
			assert(getReaderCount(load()) < 20);
			// Only works if we are the only reader
			return tryUpgradeToWriteLock();
		}

		bool tryUpgradeToWriteLock()
		{
			uint64_t w = load();
			uint64_t readerCount = getReaderCount(w);
			uint64_t writerId = getWriterId(w);
			assert(writerId == 0); // Not locked
			assert(readerCount >= 1); // At least read locked by me
			if (readerCount > 1)
				return false;
			assert(readerCount == 1);
			uint64_t neww = makeNewWord(0, RWSpinLatchThreadId);
			return compareExchangeStrong(w, neww); // Try acquire write lock when we are the last reader
		}

		void RWLockDowngradeToReadLock()
		{
			while (tryDowngradeToReadLock() == false) {
				_mm_pause();
			}
		}

		bool tryDowngradeToReadLock()
		{
			uint64_t w = load();
			uint64_t readerCount = getReaderCount(w);
			uint64_t writerId = getWriterId(w);
			assert(writerId == RWSpinLatchThreadId); // Only write-locked by me
			assert(readerCount == 0);
			uint64_t neww = makeNewWord(1, 0);
			// word.store(neww);
			// return true;
			return compareExchangeStrong(w, neww); // Try acquire write lock when we are the last reader
		}

		bool tryAcqurieWrite()
		{
			uint64_t w = load();
			uint64_t readerCount = getReaderCount(w);
			uint64_t writerId = getWriterId(w);
			assert(RWSpinLatchThreadId > 0);
			if (writerId == 0 && readerCount == 0) {
				uint64_t neww = makeNewWord(readerCount, RWSpinLatchThreadId);
				return compareExchangeStrong(w, neww);
			}
			// Locked by other writer
			return false;
		}

		bool tryReleaseRead()
		{
			uint64_t w = load();
			uint64_t readerCount = getReaderCount(w);
			uint64_t writerId = getWriterId(w);
			assert(writerId == 0);
			assert(readerCount > 0);
			uint64_t neww = makeNewWord(readerCount - 1, 0);
			// word.fetch_sub(1);
			// return true;
			return compareExchangeStrong(w, neww);
		}

		bool tryReleaseWrite()
		{
			uint64_t w = load();
			uint64_t readerCount = getReaderCount(w);
			uint64_t writerId = getWriterId(w);
			assert(writerId == RWSpinLatchThreadId);
			assert(readerCount == 0);
			uint64_t neww = makeNewWord(0, 0);
			// word.store(0);
			// return true;
			return compareExchangeStrong(w, neww);
		}

		inline uint64_t makeNewWord(uint32_t readerCount, uint32_t writerId)
		{
			uint64_t w = (((uint64_t)readerCount) << kReaderCountOffset) | (((uint64_t)writerId) << kWriterIdOffset);
			return w;
		}

		inline uint64_t getReaderCount(uint64_t w)
		{
			return (w >> kReaderCountOffset) & kReaderCountMask;
		}

		inline uint64_t getWriterId(uint64_t w)
		{
			return (w >> kWriterIdOffset) & kWriterIdMask;
		}

		static constexpr uint64_t kReaderCountOffset = 0;
		static constexpr uint64_t kReaderCountMask = 0xffffffff;
		static constexpr uint64_t kWriterIdOffset = 32;
		static constexpr uint64_t kWriterIdMask = 0xffffffff;
	};

	/**
	 * class NodeMetaData - Holds node metadata
	 */
	class NodeMetaData {
	    public:
		/** node type */
		NodeType type_;

		/** child count */
		uint16_t count_;

		NodeMetaData(NodeType nodeType)
			: type_(nodeType)
			, count_(0)
		{
		}
	};

	/**
	 * class BaseNode - Generic node class; inherited by leaf, inner node
	 */
	class alignas(8) NodeBase : public NodeLock
	{
	    private:
		NodeMetaData meta_;

	    public:
		NodeBase(NodeType nodeType)
			: NodeLock(false)
			, meta_{ nodeType }
		{
		}
		NodeType getType() const
		{
			return meta_.type_;
		}
		uint16_t getCount() const
		{
			return meta_.count_;
		}
		void setCount(uint16_t count)
		{
			RecordTreeDataWrite(&meta_.count_, sizeof(meta_.count_));
			meta_.count_ = count;
		}
	};

	/**
	 * class StackNodeElement - used when delete nodes recursively
	 */
	struct StackNodeElement {
		PersistentOffset<NodeBase> node;
		int pos;
		uint64_t version;
	};

	/**
	 * class BTreeLeaf - leaf node
	 */
	class BTreeLeaf : public NodeBase {
	    public:
		static constexpr uint64_t maxEntries = (LeafPageSize - sizeof(NodeBase) - sizeof(PersistentOffset<BTreeLeaf>) * 2) / (sizeof(KeyValuePair));
		static_assert(maxEntries >= 3, "maxEntries of BTreeLeaf must >= 3");

		PersistentOffset<BTreeLeaf> pre_;
		PersistentOffset<BTreeLeaf> next_;

		/** This is the array that we perform search on */
		KeyType keys_[maxEntries];
		ValueType values_[maxEntries];
		// KeyValuePair data_[0];

		BTreeLeaf()
			: NodeBase(NodeType::BTreeLeaf)
			, pre_(nullptr)
			, next_(nullptr)
		{
		}

		void recordEntriesRead(size_t begin, size_t count) const
		{
			if (count == 0) return;
			RecordTreeDataRead(keys_ + begin, sizeof(KeyType) * count);
			RecordTreeDataRead(values_ + begin, sizeof(ValueType) * count);
		}

		void recordEntriesWrite(size_t begin, size_t count)
		{
			if (count == 0) return;
			RecordTreeDataWrite(keys_ + begin, sizeof(KeyType) * count);
			RecordTreeDataWrite(values_ + begin, sizeof(ValueType) * count);
		}

		/**
		 * flag is true, erase the <key, valueList>
		 * otherwise, erase the <key, value>
		 *
		 * return true if erase successfully
		 */
		bool erase(int pos, const KeyComparator &keyComp_, const ValueComparator &valueComp_, bool flag)
		{
			// assert(keyComp_(keys_[pos], element.first) == 0);
			bool res = false;

			__adjust_elements_in_erase(pos);

			res = true;
			this->setCount(this->getCount() - 1);
			return res;
		}
		/**
		 * @note Used when KeyType is trivial, e.g. `OLTPBtreeFixedLenKey`.
		 */
		template <typename T = KeyType> typename std::enable_if<std::is_trivial<T>::value == true, void>::type __adjust_elements_in_erase(int pos)
		{
			const size_t moved = this->getCount() - pos - 1;
			recordEntriesRead(pos + 1, moved);
			recordEntriesWrite(pos, moved);
			memmove(keys_ + pos, keys_ + pos + 1, sizeof(KeyType) * (this->getCount() - pos - 1));
			// memmove(values_ + pos, values_ + pos + 1, sizeof(ValueType) * (this->getCount() - pos - 1));
                        for (int i = 0; i < this->getCount() - pos - 1; i++) {
                                values_[pos + i] = values_[pos + 1 + i];
                        }
			// for (uint16_t i = pos; i < this->getCount() - 1u; i++) {
			//     keys_[i] = keys_[i + 1];
			//     values_[i] = values_[i + 1];
			// }
			keys_[this->getCount() - 1].~KeyType();
			values_[this->getCount() - 1].~ValueType();
			// call dtor manually
		}
		/**
		 * @note Used when KeyType is non-trivial, e.g. `OLTPBtreeVarlenKey`.
		 */
		template <typename T = KeyType> typename std::enable_if<std::is_trivial<T>::value == false, void>::type __adjust_elements_in_erase(int pos)
		{
			assert(false);
			/*
			 * Here wants to erase key at `pos`, should transfer the pointer first.
			 */
			// char *ptr = data_[pos].first.transfer();

			// for (uint16_t i = pos; i < this->getCount() - 1u; i++) {
			//     data_[i].first = data_[i + 1].first;
			//     data_[i].second = data_[i + 1].second;
			// }
			// data_[this->getCount() - 1].~KeyValuePair();  // call dtor manually

			// /*
			//  * It is safe to delete the pointer here because already hold the
			//  * write lock of the leaf page.
			//  */
			// delete[] ptr;
		}

		bool hasEnoughSpace(int need)
		{
			return this->getCount() + need <= (int)maxEntries;
		}

		/**
		 * merge right `sibling`
		 * `sibling` need to be reclaimed
		 */
		void merge(BTreeLeaf *sibling, const TreeNodeAllocation &allocation)
		{
			assert(hasEnoughSpace(sibling->getCount()));
			const uint16_t base = this->getCount();
			const uint16_t incoming = sibling->getCount();
			sibling->recordEntriesRead(0, incoming);
			recordEntriesWrite(base, incoming);
			for (uint16_t i = this->getCount(); i < sibling->getCount() + this->getCount(); i++) {
				new (&keys_[i]) KeyType{ sibling->keys_[i - this->getCount()] }; // Placement new
				new (&values_[i]) ValueType{ sibling->values_[i - this->getCount()] };
				sibling->keys_[i - this->getCount()].~KeyType(); // call dtor manually
				sibling->values_[i - this->getCount()].~ValueType();
			}
			this->setCount(this->getCount() + sibling->getCount());
			RecordTreeDataRead(&sibling->next_, sizeof(sibling->next_));
			RecordTreeDataWrite(&this->next_, sizeof(this->next_));
			this->next_ = sibling->next_.get();
                        if (this->next_.get()) {
				RecordTreeDataWrite(&sibling->next_->pre_,
				                    sizeof(sibling->next_->pre_));
                                sibling->next_->pre_ = this;
			}
			assert(((uint64_t)sibling) != 0xffffffffffffffffull);
			allocation.Retire(sibling, kLeafPageSize);
		}

		bool needMerge()
		{
			return this->getCount() / (maxEntries + 0.0) < kMergeThreshold;
		}

		int getSurplus()
		{
			int threshold = std::ceil(maxEntries * kMergeThreshold);
			return this->getCount() - threshold;
		}

		bool isFull()
		{
			return this->getCount() == maxEntries;
		}

		ValueType getValue(const KeyType &k, const KeyComparator &keyComp_, std::function<ValueType(void)> createValue, bool &success)
		{
			assert(this->getCount() < maxEntries);
			unsigned pos = -1;
			pos = lowerBound(k, keyComp_);
			if (pos < this->getCount() && keyComp_(keys_[pos], k) == 0) {
				// already exists
				success = false;
				RecordTreeDataRead(&values_[pos], sizeof(ValueType));
				return values_[pos];
			} else {
				const uint16_t count = this->getCount();
				recordEntriesRead(pos, count - pos);
				recordEntriesWrite(pos, count - pos + 1);
				if (pos == this->getCount()) {
					// Placement new
					new (&keys_[this->getCount()]) KeyType{ k };
					new (&values_[this->getCount()]) ValueType{ createValue() };
				} else {
					// Placement new
					new (&keys_[this->getCount()]) KeyType{ keys_[this->getCount() - 1] };
					new (&values_[this->getCount()]) ValueType{ values_[this->getCount() - 1] };
					for (uint16_t i = this->getCount() - 1; i > pos; i--) {
						keys_[i] = keys_[i - 1];
						values_[i] = values_[i - 1];
					}
					keys_[pos] = k;
					values_[pos] = createValue();
				}
				this->setCount(this->getCount() + 1);
				success = true;
				RecordTreeDataRead(&values_[pos], sizeof(ValueType));
				return values_[pos];
			}
		}

		unsigned lowerBound(const KeyType &k, const KeyComparator &keyComp_)
		{
			if (this->getCount() < 128) {
				int left = 0;
				while (left < this->getCount()) {
					RecordTreeDataRead(&this->keys_[left], sizeof(KeyType));
					if (keyComp_(this->keys_[left], k) >= 0) break;
					++left;
				}
				return left;
			} else {
				int left = 0, right = this->getCount() - 1;
				while (left <= right) {
					int mid = left + (right - left) / 2;
					RecordTreeDataRead(&keys_[mid], sizeof(KeyType));
					int comp = keyComp_(keys_[mid], k);
					if (comp == 0)
						return mid;
					else if (comp < 0)
						left = mid + 1;
					else
						right = mid - 1;
				}
				return left;
			}
		}

		__attribute__((deprecated)) void update(const KeyType &k, ValueType p, unsigned pos)
		{
			assert(keyComp_(keys_[pos], k) == 0);
			RecordTreeDataWrite(&values_[pos], sizeof(ValueType));
			values_[pos] = p;
		}

		const KeyType &max_key()
		{
			assert(this->getCount());
			RecordTreeDataRead(&keys_[this->getCount() - 1], sizeof(KeyType));
			return keys_[this->getCount() - 1];
		}

		// void insert_at(KeyType k, ValueType p, unsigned pos) {
		//     assert(keyComp_(data_[pos].first, k) == 0);
		//     if (this->getCount()) {
		//         for (uint16_t i = this->getCount(); i > pos; i--) {
		//             data_[i].first = data_[i - 1].first;
		//             data_[i].second = data_[i - 1].second;
		//         }
		//         data_[pos].first = k;
		//         data_[pos].second = p;
		//     } else {
		//         data_[0].first = k;
		//         data_[0].second = p;
		//     }
		//     setCount(this->getCount() + 1);
		// }

		/**
		 * return v (in the b+tree) and false , if <k, v> already exists
		 * otherwise return v and true
		 */
		ValueType insert(const KeyType &k, ValueType v, const KeyComparator &keyComp_, bool &success)
		{
			assert(this->getCount() < maxEntries);
			unsigned pos = -1;
			pos = lowerBound(k, keyComp_);
			if (pos < this->getCount() && keyComp_(keys_[pos], k) == 0) {
				// already exists
				success = false;
				RecordTreeDataRead(&values_[pos], sizeof(ValueType));
				return values_[pos];
			} else {
				const uint16_t count = this->getCount();
				recordEntriesRead(pos, count - pos);
				recordEntriesWrite(pos, count - pos + 1);
				if (pos == this->getCount()) {
					// Placement new
					new (&keys_[this->getCount()]) KeyType{ k };
					new (&values_[this->getCount()]) ValueType{ v };
				} else {
					// Placement new
					new (&keys_[this->getCount()]) KeyType{ keys_[this->getCount() - 1] };
					new (&values_[this->getCount()]) ValueType{ values_[this->getCount() - 1] };
					for (uint16_t i = this->getCount() - 1; i > pos; i--) {
						keys_[i] = keys_[i - 1];
						values_[i] = values_[i - 1];
					}
					keys_[pos] = k;
					values_[pos] = v;
				}
				this->setCount(this->getCount() + 1);
				success = true;
				return v;
			}
		}

		/**
		 * return v (in the b+tree) and false , if <k, v> already exists
		 * otherwise return v and true
		 */
		bool insert(const KeyType &k, const ValueType &v, const KeyComparator &keyComp_)
		{
			assert(this->getCount() < maxEntries);
			unsigned pos = -1;
			pos = lowerBound(k, keyComp_);
			if (pos < this->getCount() && keyComp_(keys_[pos], k) == 0) {
				// already exists
				return false;
			} else {
				const uint16_t count = this->getCount();
				recordEntriesRead(pos, count - pos);
				recordEntriesWrite(pos, count - pos + 1);
				if (pos == this->getCount()) {
					// Placement new
					new (&keys_[this->getCount()]) KeyType{ k };
					new (&values_[this->getCount()]) ValueType{ v };
				} else {
					// Placement new
					new (&keys_[this->getCount()]) KeyType{ keys_[this->getCount() - 1] };
					new (&values_[this->getCount()]) ValueType{ values_[this->getCount() - 1] };
					for (uint16_t i = this->getCount() - 1; i > pos; i--) {
						keys_[i] = keys_[i - 1];
						values_[i] = values_[i - 1];
					}
					keys_[pos] = k;
					values_[pos] = v;
				}
				this->setCount(this->getCount() + 1);
				return true;
			}
		}

		/**
		 * split leaf node
		 */
		BTreeLeaf *split(KeyType &sep, const TreeNodeAllocation &allocation)
		{
			char *base = reinterpret_cast<char *>(allocation.Allocate(LeafPageSize));
			BTreeLeaf *newLeaf = new (base) BTreeLeaf(); // Placement new
			RecordTreeAccess(allocation, newLeaf, true);

			newLeaf->setCount(this->getCount() - (this->getCount() / 2));
			const uint16_t moved = newLeaf->getCount();
			newLeaf->next_ = next_.get();
			newLeaf->pre_ = this;
			RecordTreeDataRead(&next_, sizeof(next_));
			RecordTreeDataWrite(&newLeaf->next_, sizeof(newLeaf->next_));
			RecordTreeDataWrite(&newLeaf->pre_, sizeof(newLeaf->pre_));

			this->setCount(this->getCount() - newLeaf->getCount());
			recordEntriesRead(this->getCount(), moved);
			newLeaf->recordEntriesWrite(0, moved);
			RecordTreeDataWrite(&next_, sizeof(next_));
			next_ = newLeaf;
			// Master's split forgot the backward link: the old right sibling
			// still points at this leaf, so leaf-first-key callbacks observe a
			// stale predecessor and the migrated prev/next bits stay wrong
			// (YCSB-E scan probe livelock). Mirror the erase-merge back-link.
			if (newLeaf->next_.get() != nullptr) {
				RecordTreeDataWrite(&newLeaf->next_->pre_,
				                    sizeof(newLeaf->next_->pre_));
				newLeaf->next_->pre_ = newLeaf;
			}

			for (uint16_t i = 0; i < newLeaf->getCount(); i++) {
				new (&newLeaf->keys_[i]) KeyType{ keys_[i + this->getCount()] }; // Placement new
				new (&newLeaf->values_[i]) ValueType{ values_[i + this->getCount()] }; // Placement new
				keys_[i + this->getCount()].~KeyType();
				values_[i + this->getCount()].~ValueType(); // call dtor manually
			}

			__get_separate_key_in_split(sep);

			return newLeaf;
		}
		/**
		 * @note Used when KeyType is trivial, e.g. `OLTPBtreeFixedLenKey`.
		 */
		template <typename T = KeyType> typename std::enable_if<std::is_trivial<T>::value == true, void>::type __get_separate_key_in_split(KeyType &sep)
		{
			RecordTreeDataRead(&keys_[this->getCount() - 1], sizeof(KeyType));
			sep = keys_[this->getCount() - 1];
		}
		/**
		 * @note Used when KeyType is non-trivial, e.g. `OLTPBtreeVarlenKey`.
		 */
		template <typename T = KeyType>
		typename std::enable_if<std::is_trivial<T>::value == false, void>::type __get_separate_key_in_split(KeyType &sep)
		{
			assert(false);
			/*
			 * separate key will push up to the node's parent, so here should
			 * deep copy the key, because do NOT allow two non-trivial KeyType
			 * share the same pointer, otherwise will cause "double free".
			 */
			sep = keys_[this->getCount() - 1].deepCopy();
		}
	};
	static_assert(LeafPageSize >= sizeof(BTreeLeaf), "LeafPageSize too small");

	/**
	 * BTreeInner - inner node
	 */
	class BTreeInner : public NodeBase {
	    public:
		static constexpr uint64_t maxEntries = (InnerPageSize - sizeof(NodeBase)) / (sizeof(KeyType) + sizeof(PersistentOffset<NodeBase>));
		static_assert(maxEntries >= 3, "maxEntries of BTreeInner must >= 3");

		static constexpr uint64_t childOffset = maxEntries * sizeof(KeyType);

		/**
		 * Layout of `data_`:
		 *  -------------------------------------------------------------------
		 *  | Key 0 | Key 1 | ... | Key N | Child 0 | Child 1 | ... | Child N |
		 *  -------------------------------------------------------------------
		 */
		char data_[0];

		BTreeInner()
			: NodeBase(NodeType::BTreeInner)
		{
		}

		KeyType &keyAt(size_t i)
		{
			auto *key = reinterpret_cast<KeyType *>(
			    reinterpret_cast<intptr_t>(data_) + i * sizeof(KeyType));
			RecordTreeDataRead(key, sizeof(KeyType));
			return *key;
		}

		PersistentOffset<NodeBase> &childAt(size_t i)
		{
			auto *child =
			    reinterpret_cast<PersistentOffset<NodeBase> *>(
			        reinterpret_cast<intptr_t>(data_) + childOffset +
			        i * sizeof(PersistentOffset<NodeBase>));
			RecordTreeDataRead(
			    child, sizeof(PersistentOffset<NodeBase>));
			return *child;
		}

		void setKeyAt(size_t i, const KeyType &key)
		{
			auto *slot = reinterpret_cast<KeyType *>(
			    reinterpret_cast<intptr_t>(data_) + i * sizeof(KeyType));
			RecordTreeDataWrite(slot, sizeof(KeyType));
			*slot = key;
		}

		void setChildAt(size_t i, NodeBase *child)
		{
			auto *slot =
			    reinterpret_cast<PersistentOffset<NodeBase> *>(
			        reinterpret_cast<intptr_t>(data_) + childOffset +
			        i * sizeof(PersistentOffset<NodeBase>));
			RecordTreeDataWrite(
			        slot, sizeof(PersistentOffset<NodeBase>));
			*slot = child;
		}

		void newKey(const size_t pos, const KeyType &key)
		{
			auto *slot = reinterpret_cast<KeyType *>(
			    reinterpret_cast<intptr_t>(data_) + pos * sizeof(KeyType));
			RecordTreeDataWrite(slot, sizeof(KeyType));
			new (slot) KeyType{ key }; // Placement new
		}

		bool isFull()
		{
			return this->getCount() == (maxEntries - 1);
		}

		/**
		 * erase the key at `pos` and the child at `pos + 1`
		 * return whether a merge opt is required
		 * When the type and number of nodes are InnerNode and 0 respectively,
		 * you need to change root node, old root node need to be reclaimed
		 *
		 * NOTE: return true, if need to change root node
		 */
		bool erase(int pos, const TreeNodeAllocation &allocation)
		{
			__adjust_elements_in_erase(pos, allocation);

			this->setCount(this->getCount() - 1);
			return this->getCount() == 0;
		}
		/**
		 * @note Used when KeyType is trivial, e.g. `OLTPBtreeFixedLenKey`.
		 */
		template <typename T = KeyType> typename std::enable_if<std::is_trivial<T>::value == true, void>::type __adjust_elements_in_erase(int pos, const TreeNodeAllocation &allocation)
		{
			for (int i = pos; i < this->getCount() - 1; i++) {
				setKeyAt(i, keyAt(i + 1));
			}
			keyAt(this->getCount() - 1).~KeyType(); // call dtor manually

			// always merge nodes to the left node, so remove the `pos + 1` child
			// memmove(&childAt(pos + 1), &childAt(pos + 2), sizeof(RegionOffset) * (this->getCount() - pos - 1));
                        for (int i = 0; i < this->getCount() - pos - 1; i++) {
                                setChildAt(pos + 1 + i, childAt(pos + 2 + i).get());
                        }
		}
		/**
		 * @note Used when KeyType is non-trivial, e.g. `OLTPBtreeVarlenKey`.
		 */
		template <typename T = KeyType> typename std::enable_if<std::is_trivial<T>::value == false, void>::type __adjust_elements_in_erase(int pos, const TreeNodeAllocation &allocation)
		{
			assert(false);
			/*
			 * Here wants to erase key at `pos`, should transfer the pointer first.
			 */
			char *ptr = keyAt(pos).transfer();

			for (int i = pos; i < this->getCount() - 1; i++) {
				setKeyAt(i, keyAt(i + 1));
			}
			keyAt(this->getCount() - 1).~KeyType(); // call dtor manually

			/*
			 * Because of OLC strategy, can NOT delete pointer here, should transfer
			 * the pointer to EBR, it will delete the pointer safely.
			 */
			assert(((uint64_t)ptr) != 0xffffffffffffffffull);
			allocation.Retire(ptr, kLeafPageSize);

			// always merge nodes to the left node, so remove the `pos + 1` child
			// memmove(&childAt(pos + 1), &childAt(pos + 2), sizeof(RegionOffset) * (this->getCount() - pos - 1));
                        for (int i = 0; i < this->getCount() - pos - 1; i++) {
                                setChildAt(pos + 1 + i, childAt(pos + 2 + i).get());
                        }
		}

		void merge(BTreeInner *sibling, const KeyType &subTreeMaxKey, const TreeNodeAllocation &allocation)
		{
			__adjust_elements_in_merge(sibling, subTreeMaxKey);

			this->setCount(this->getCount() + 1);
			this->setCount(this->getCount() + sibling->getCount());

			assert(((uint64_t)sibling) != 0xffffffffffffffffull);
			allocation.Retire(sibling, kLeafPageSize);
		}
		/**
		 * @note Used when KeyType is trivial, e.g. `OLTPBtreeFixedLenKey`.
		 */
		template <typename T = KeyType>
		typename std::enable_if<std::is_trivial<T>::value == true, void>::type __adjust_elements_in_merge(BTreeInner *sibling,
														  const KeyType &subTreeMaxKey)
		{
			newKey(this->getCount(), subTreeMaxKey);

			for (uint16_t i = 0; i < sibling->getCount(); i++) {
				newKey(this->getCount() + 1 + i, sibling->keyAt(i));
				sibling->keyAt(i).~KeyType(); // call dtor manually
			}

			// memmove(&childAt(this->getCount() + 1), &sibling->childAt(0), sizeof(RegionOffset) * (sibling->getCount() + 1));
                        for (int i = 0; i < sibling->getCount() + 1; i++) {
                                setChildAt(this->getCount() + 1 + i,
                                           sibling->childAt(i).get());
                        }
		}
		/**
		 * @note Used when KeyType is non-trivial, e.g. `OLTPBtreeVarlenKey`.
		 */
		template <typename T = KeyType>
		typename std::enable_if<std::is_trivial<T>::value == false, void>::type __adjust_elements_in_merge(BTreeInner *sibling,
														   const KeyType &subTreeMaxKey)
		{
			/*
			 * Here should deep copy `subTreeMaxKey`, because do NOT allow two
			 * non-trivial KeyType share the same pointer, otherwise will cause
			 * "double free".
			 */
			newKey(this->getCount(), subTreeMaxKey.deepCopy());

			for (uint16_t i = 0; i < sibling->getCount(); i++) {
				newKey(this->getCount() + 1 + i, sibling->keyAt(i));
				sibling->keyAt(i).~KeyType(); // call dtor manually
			}

			// memmove(&childAt(this->getCount() + 1), &sibling->childAt(0), sizeof(RegionOffset) * (sibling->getCount() + 1));
                        for (int i = 0; i < sibling->getCount() + 1; i++) {
                                setChildAt(this->getCount() + 1 + i,
                                           sibling->childAt(i).get());
                        }
		}

		bool hasEnoughSpace(int need)
		{
			return this->getCount() + need + 1 < (int)maxEntries;
		}

		unsigned lowerBound(const KeyType &k, const KeyComparator &keyComp_)
		{
			if (this->getCount() < 128) {
				int left = 0;
				while (left < this->getCount() && keyComp_(keyAt(left), k) < 0)
					++left;
				return left;
			} else {
				int left = 0, right = this->getCount() - 1;
				while (left <= right) {
					int mid = left + (right - left) / 2;
					int comp = keyComp_(keyAt(mid), k);
					if (comp == 0)
						return mid;
					else if (comp < 0)
						left = mid + 1;
					else
						right = mid - 1;
				}
				return left;
			}
		}

		bool needMerge()
		{
			return this->getCount() / (maxEntries + 0.0) < kMergeThreshold;
		}

		int getSurplus()
		{
			int threshold = std::ceil(maxEntries * kMergeThreshold);
			return this->getCount() - threshold;
		}

		BTreeInner *split(KeyType &sep, const TreeNodeAllocation &allocation)
		{
			char *base = reinterpret_cast<char *>(allocation.Allocate(InnerPageSize));
			BTreeInner *newInner = new (base) BTreeInner(); // Placement new
			RecordTreeAccess(allocation, newInner, true);

			newInner->setCount(this->getCount() - (this->getCount() / 2));
			this->setCount(this->getCount() - newInner->getCount() - 1);

			sep = keyAt(this->getCount());
			/*
			 * separate key will push up to the node's parent,
			 * so need to destruct separate key here and NO need
			 * to transfer pointer.
			 */
			keyAt(this->getCount()).~KeyType(); // call dtor manually

			for (uint16_t i = 0; i < newInner->getCount(); i++) {
				newInner->newKey(i, keyAt(this->getCount() + 1 + i));
				keyAt(this->getCount() + 1 + i).~KeyType(); // call dtor manually
			}

			// memcpy(&newInner->childAt(0), &childAt(this->getCount() + 1), sizeof(RegionOffset) * (newInner->getCount() + 1));
                        for (int i = 0; i < newInner->getCount() + 1; i++) {
                                newInner->setChildAt(i, childAt(this->getCount() + 1 + i).get());
                        }

			return newInner;
		}

		const KeyType &max_key()
		{
			assert(this->getCount());
			return keyAt(this->getCount() - 1);
		}

		void insert(const KeyType &k, NodeBase *child, const KeyComparator &keyComp_)
		{
			assert(this->getCount() < maxEntries - 1);
			unsigned pos = lowerBound(k, keyComp_);
			if (pos == this->getCount()) {
				newKey(this->getCount(), k);
			} else {
				newKey(this->getCount(), keyAt(this->getCount() - 1));
				for (uint16_t i = this->getCount() - 1; i > pos; i--) {
					setKeyAt(i, keyAt(i - 1));
				}
				setKeyAt(pos, k);
			}

			// memmove(&childAt(pos + 1), &childAt(pos), sizeof(RegionOffset) * (this->getCount() - pos + 1));
                        for (int i = this->getCount() - pos; i >= 0; i--) {
                                setChildAt(pos + 1 + i, childAt(pos + i).get());
                        }
			setChildAt(pos, child);

			auto displaced = childAt(pos + 1);
			auto inserted = childAt(pos);
			setChildAt(pos, displaced.get());
			setChildAt(pos + 1, inserted.get());
			this->setCount(this->getCount() + 1);
		}

	};
	static_assert(InnerPageSize > sizeof(BTreeInner), "InnerPageSize too small");

	/**
	 * NOTE: developing
	 */
	class BPlusTreeIterator {
		/** Enum that represents the state of the iterator */
		enum IteratorState { VALID, END, REND, RETRY1, INVALID };

		PersistentOffset<BTreeLeaf> curNode_;
		int curPos_;
		IteratorState state_;

	    public:
		BPlusTreeIterator()
			: curNode_(nullptr)
			, curPos_(-1)
			, state_(VALID)
			, allocation_(nullptr)
		{
		}

		~BPlusTreeIterator()
		{
			if (curNode_.get()) {
				curNode_->iteratorLeave();
			}
		}

		/**
		 * Assuming that curNode has increase the number of readers by one
		 */
		BPlusTreeIterator(BTreeLeaf *curNode, int curPos,
		                  const TreeNodeAllocation *allocation = nullptr)
			: curNode_(curNode)
			, curPos_(curPos)
			, state_(VALID)
			, allocation_(allocation)
		{
			if (curNode_.get() == nullptr || curNode_->getCount() == curPos) {
				setEndIterator(true);
				return;
			}
			state_ = IteratorState::VALID;
			assert(curPos >= 0);
		}

		BPlusTreeIterator &operator=(const BPlusTreeIterator &) = delete;

		void operator++(int)
		{
			forward();
		}

		void operator++()
		{
			forward();
		}

		void operator--(int)
		{
			backward();
		}

		void operator--()
		{
			backward();
		}

		/**
		 * Equality operator to check if two iterators are equal
		 * @param itr Iterator to be compared with this iterator
		 * @return true if iterators are equal, false otherwise
		 *
		 * NOTE: developing
		 */
		bool operator==(const BPlusTreeIterator &itr)
		{
			bool result = (curNode_.get() == itr.curNode_.get() && curPos_ == itr.curPos_ && state_ == itr.state_);
			return result;
		}

		bool operator!=(const BPlusTreeIterator &itr)
		{
			bool result = (curNode_.get() == itr.curNode_.get() && curPos_ == itr.curPos_ && state_ == itr.state_);
			return !result;
		}

		const KeyType &key() const
		{
			RecordTreeDataRead(&curNode_->keys_[curPos_], sizeof(KeyType));
			return curNode_->keys_[curPos_];
		}

		const ValueType &value() const
		{
			RecordTreeDataRead(&curNode_->values_[curPos_], sizeof(ValueType));
			return curNode_->values_[curPos_];
		}

		/**
		 * Resets the iterator to represent a dummy iterator.
		 */
		void resetIterator(bool itrLeave)
		{
			if (curNode_.get() && itrLeave) {
				curNode_->iteratorLeave();
			}
			curNode_ = nullptr;
			curPos_ = -1;
		}

		/**
		 * Sets the state of the iterator to END iterator
		 */
		void setEndIterator(bool itrLeave = false)
		{
			resetIterator(itrLeave);
			state_ = IteratorState::END;
		}

		/**
		 * Sets the state of the iterator to RETRY iterator
		 */
		void setRetryIterator(bool itrLeave = false)
		{
			resetIterator(itrLeave);
			state_ = IteratorState::RETRY1;
		}

		/**
		 * Returns End() iterator
		 */
		static BPlusTreeIterator getEndIterator()
		{
			auto iterator = BPlusTreeIterator();
			iterator.setEndIterator();
			return iterator;
		}

		/**
		 * Returns Retry() iterator
		 */
		static BPlusTreeIterator getRetryIterator()
		{
			auto iterator = BPlusTreeIterator();
			iterator.setRetryIterator();
			return iterator;
		}

	    private:
		/**
		 * called when iterator move to the next leaf node
		 */
		void forward()
		{
			assert(state_ == IteratorState::VALID);
			bool needRestart = false;
			curPos_++;
			// move to next leafNode
			if (curPos_ >= static_cast<int>(curNode_->getCount())) {
				RecordTreeDataRead(&curNode_->next_, sizeof(curNode_->next_));
				if (curNode_->next_.get() == nullptr) {
					setEndIterator(true);
					return;
				}
				auto preNode = curNode_.get();
				curNode_ = curNode_->next_.get();
				if (allocation_ != nullptr)
					RecordTreeAccess(*allocation_, curNode_.get(), false);
				curNode_->iteratorEnter(needRestart);
				if (needRestart) {
					preNode->iteratorLeave();
					setRetryIterator();
					return;
				}
				preNode->iteratorLeave();
				curPos_ = 0;
			}
		}

		/**
		 * called when iterator move to the previous leaf node
		 */
		void backward()
		{
			assert(state_ == IteratorState::VALID);
			bool needRestart = false;
			curPos_--;
			// move to previous leafNode
			if (curPos_ < 0) {
				RecordTreeDataRead(&curNode_->pre_, sizeof(curNode_->pre_));
				if (curNode_->pre_.get() == nullptr) {
					setEndIterator(true);
					return;
				}
				auto preNode = curNode_.get();
				curNode_ = curNode_->pre_.get();
				if (allocation_ != nullptr)
					RecordTreeAccess(*allocation_, curNode_.get(), false);
				curNode_->iteratorEnter(needRestart);
				if (needRestart) {
					preNode->iteratorLeave();
					setRetryIterator();
					return;
				}
				preNode->iteratorLeave();
				curPos_ = curNode_->getCount() - 1;
			}
		}

		const TreeNodeAllocation *allocation_{ nullptr };
	};

	BPlusTree(const TreeNodeAllocation &allocation, bool isUnique = false, const KeyComparator &keyComp = KeyComparator{}, const ValueComparator &valueComp = ValueComparator{})
		: keyComp_(keyComp)
		, valueComp_(valueComp)
		, keyUnique_(isUnique)
		, allocation_(allocation)
	{
		TreeAccessScope access_scope(allocation_);
		char *base = reinterpret_cast<char *>(allocation_.Allocate(LeafPageSize));
		store_root(new (base) BTreeLeaf()); // Placement new
		stats_.leaf_nodes++;
	}

	// Attach from a previously published root pointer (private trees) or bind
	// an HWCC atomic RegionOffset slot afterward via bind_published_root.
	BPlusTree(const TreeNodeAllocation &allocation, void *persisted_root,
	          bool isUnique = false, const KeyComparator &keyComp = KeyComparator{},
	          const ValueComparator &valueComp = ValueComparator{})
		: keyComp_(keyComp)
		, valueComp_(valueComp)
		, keyUnique_(isUnique)
		, allocation_(allocation)
	{
		TreeAccessScope access_scope(allocation_);
		if (persisted_root == nullptr) {
			throw std::invalid_argument("BPlusTree attach requires a persisted root");
		}
		root_.store(allocation_.ToOffset(persisted_root),
		            std::memory_order_release);
	}

	// Shared CXL trees: publish/load the live root via an HWCC atomic offset so
	// already-attached peers observe makeRoot / root merges (original shared
	// visible-root semantics). Private trees leave published_root_ null.
	void bind_published_root(std::atomic<tigonkv::engine::RegionOffset> *slot)
	{
		TreeAccessScope access_scope(allocation_);
		if (slot == nullptr)
			throw std::invalid_argument("BPlusTree published root slot is null");
		const auto off = TreeAtomicLoad(*slot, std::memory_order_acquire);
		if (off == tigonkv::engine::kNullOffset) {
			// Creator: publish the process-local root allocated by the ctor.
			const auto local_offset =
			    root_.load(std::memory_order_acquire);
			NodeBase *local = local_offset == tigonkv::engine::kNullOffset
			    ? nullptr
			    : static_cast<NodeBase *>(allocation_.FromOffset(local_offset));
			if (local == nullptr)
				throw std::runtime_error("BPlusTree has no local root to publish");
			published_root_ = slot;
			TreeAtomicStore(*slot, allocation_.ToOffset(local),
			                std::memory_order_release);
		} else {
			// Attacher: adopt the HWCC live root.
			published_root_ = slot;
			root_.store(off, std::memory_order_release);
		}
	}

	tigonkv::engine::RegionOffset root_offset_for_persistence() const
	{
		return allocation_.ToOffset(load_root());
	}

	void makeRoot(const KeyType &k, NodeBase *leftChild, NodeBase *rightChild)
	{
		TreeAccessScope access_scope(allocation_);
		char *base = reinterpret_cast<char *>(allocation_.Allocate(InnerPageSize));
		auto inner = new (base) BTreeInner(); // Placement new
		RecordTreeAccess(allocation_, inner, true);

		inner->setCount(1);
		inner->newKey(0, k);
		inner->setChildAt(0, leftChild);
		inner->setChildAt(1, rightChild);
		store_root(inner);
	}

	// This is a process-local, non-owning view of nodes allocated in a mapped
	// region.  EBR owns persistent reclamation; destroying the view must never
	// recurse through a shared tree.
	~BPlusTree() = default;

	int intRand(const int &min, const int &max)
	{
		static thread_local std::mt19937 generator;
		std::uniform_int_distribution<int> distribution(min, max);
		return distribution(generator);
	}

	void yield(int count, bool wait = false)
	{
		if (count > 10 && wait) { // too many collisions/contention, switch to randomized waits (TODO: exponential back-off)
#ifndef WINDOWS
			auto us = intRand(0, 20);
			std::this_thread::sleep_for(std::chrono::microseconds(us));
#else
			// TODO in windows
#endif
		} else if (count > 3) {
#ifndef WINDOWS
			sched_yield();
#endif
		} else {
			_mm_pause();
		}
	}

	bool hasEnoughSpace(NodeBase *node, int count)
	{
		if (node->getType() == NodeType::BTreeInner) {
			return static_cast<BTreeInner *>(node)->hasEnoughSpace(count);
		} else {
			return static_cast<BTreeLeaf *>(node)->hasEnoughSpace(count);
		}
	}

	bool needMerge(NodeBase *node, int count)
	{
		if (node->getType() == NodeType::BTreeInner) {
			return static_cast<BTreeInner *>(node)->needMerge();
		} else {
			return static_cast<BTreeLeaf *>(node)->needMerge();
		}
	}

	int getSurplus(NodeBase *node)
	{
		if (node->getType() == NodeType::BTreeInner) {
			return static_cast<BTreeInner *>(node)->getSurplus();
		} else {
			return static_cast<BTreeLeaf *>(node)->getSurplus();
		}
	}
	/**
	 * borrow data from sibling
	 * when borrow from left, opt is 0
	 * when borrow from right, opt is 1
	 * `pos`: the left node pos in the parent node
	 *        needed when update parent node
	 */
	void reallocNode(NodeBase *left, NodeBase *right, int opt, BTreeInner *p, unsigned pos)
	{
		if (left->getType() == NodeType::BTreeLeaf) {
			auto a = static_cast<BTreeLeaf *>(left);
			auto b = static_cast<BTreeLeaf *>(right);
			if (opt) {
				/*
				 * left borrow one from right
				 *
				 *        A                              C
				 *      /   \                         /     \
				 *  [ B ]   [ C  D ]     ==>      [ B  C ]  [ D ]
				 *    |       |  |                  |  |      |
				 *    b       c  d                  b  c      d
				 */
				// adjust left node
				b->recordEntriesRead(0, 1);
				a->recordEntriesWrite(a->getCount(), 1);
				new (&a->keys_[a->getCount()]) KeyType{ b->keys_[0] }; // Placement new
				new (&a->values_[a->getCount()]) ValueType{ b->values_[0] }; // Placement new
				a->setCount(a->getCount() + 1);

				// adjust right node
				b->recordEntriesRead(1, b->getCount() - 1);
				b->recordEntriesWrite(0, b->getCount() - 1);
				for (uint16_t i = 0; i < b->getCount() - 1; i++) {
					b->keys_[i] = b->keys_[i + 1];
					b->values_[i] = b->values_[i + 1];
				}
				b->keys_[b->getCount() - 1].~KeyType(); // call dtor manually
				b->values_[b->getCount() - 1].~ValueType();
				b->setCount(b->getCount() - 1);
			} else {
				/*
				 * right borrow one from left
				 *
				 *         A                           B
				 *      /     \                     /     \
				 *  [ B  C ]  [ D ]     ==>      [ B ]   [ C D ]
				 *    |  |      |                  |       |  |
				 *    b  c      d                  b       c  d
				 */
				// move right
				b->recordEntriesRead(0, b->getCount());
				b->recordEntriesWrite(1, b->getCount());
				for (uint16_t i = b->getCount(); i > 0; i--) {
					new (&b->keys_[i]) KeyType{ b->keys_[i - 1] };
					new (&b->values_[i]) ValueType{ b->values_[i - 1] };
					b->keys_[i - 1].~KeyType();
					b->values_[i - 1].~ValueType();
				}
				b->setCount(b->getCount() + 1);
				a->setCount(a->getCount() - 1);
				a->recordEntriesRead(a->getCount(), 1);
				b->recordEntriesWrite(0, 1);
				for (int i = 0; i < 1; i++) {
					new (&b->keys_[i]) KeyType{ a->keys_[a->getCount() + i] };
					new (&b->values_[i]) ValueType{ a->values_[a->getCount() + i] };
					a->keys_[a->getCount() + i].~KeyType();
					a->values_[a->getCount() + i].~ValueType();
				}
			}
			// adjust parent: replace the parent key which in the `pos`
			__adjust_parent_in_reallocNode(p, pos, a->max_key());
		} else {
			auto a = static_cast<BTreeInner *>(left);
			auto b = static_cast<BTreeInner *>(right);
			if (opt) {
				/*
				 * left borrow one from right
				 *
				 *        A                               C
				 *      /   \                         /       \
				 *  [ B ]   [ C  D ]     ==>      [B  A ]    [ D ]
				 *  /  \    /  \  \              /  \  \     /  \
				 *  a    b  c    d   e          a    b  c   d    e
				 */
				// adjust left node
				a->setCount(a->getCount() + 1);
				a->newKey(a->getCount() - 1, p->keyAt(pos));
				a->setChildAt(a->getCount(), b->childAt(0).get());

				// adjust parent
				p->setKeyAt(pos, b->keyAt(0));

				// adjust right node
				// memmove(&b->childAt(0), &b->childAt(1), sizeof(RegionOffset) * (b->getCount()));
                                for (int i = 0; i < b->getCount(); i++) {
                                        b->setChildAt(i, b->childAt(1 + i).get());
                                }
				for (uint16_t i = 0; i < b->getCount() - 1; i++) {
					b->setKeyAt(i, b->keyAt(i + 1));
				}
				b->keyAt(b->getCount() - 1).~KeyType(); // call dtor manually
				b->setCount(b->getCount() - 1);
			} else {
				/*
				 * right borrow one from left
				 *
				 *         A                             C
				 *      /     \                       /     \
				 *  [ B C ]   [ D ]      ==>      [ B ]    [ A D ]
				 *  /  \  \    / \                 /  \     / / \
				 *  a   b  c  d   e               a   b    c d   e
				 */
				// adjust right node
				// memmove(&b->childAt(1), &b->childAt(0), sizeof(RegionOffset) * (b->getCount() + 1));
                                for (int i = b->getCount(); i >= 0; i--) {
                                        b->setChildAt(1 + i, b->childAt(i).get());
                                }
				for (int i = b->getCount(); i > 0; i--) {
					b->newKey(i, b->keyAt(i - 1));
					b->keyAt(i - 1).~KeyType();
				}
				b->newKey(0, p->keyAt(pos));
				b->setChildAt(0, a->childAt(a->getCount()).get());
				b->setCount(b->getCount() + 1);

				// adjust parent
				p->setKeyAt(pos, a->keyAt(a->getCount() - 1));

				// adjust left node
				a->keyAt(a->getCount() - 1).~KeyType();
				a->setCount(a->getCount() - 1);
			}
		}
	}
	/**
	 * @note Used when KeyType is trivial, e.g. `OLTPBtreeFixedLenKey`.
	 */
	template <typename T = KeyType>
	typename std::enable_if<std::is_trivial<T>::value == true, void>::type __adjust_parent_in_reallocNode(BTreeInner *p, unsigned pos, const KeyType &key)
	{
		p->setKeyAt(pos, key);
	}
	/**
	 * @note Used when KeyType is non-trivial, e.g. `OLTPBtreeVarlenKey`.
	 */
	template <typename T = KeyType>
	typename std::enable_if<std::is_trivial<T>::value == false, void>::type __adjust_parent_in_reallocNode(BTreeInner *p, unsigned pos, const KeyType &key)
	{
		assert(false);
		char *ptr = p->keyAt(pos).transfer();

		p->setKeyAt(pos, key.deepCopy());

		assert(((uint64_t)ptr) != 0xffffffffffffffffull);
		allocation_.Retire(ptr, kLeafPageSize);
	}

	/**
	 * return v if insert successful
	 * otherwise return an existing value
	 */
	bool insert(const KeyType &k, const ValueType &v)
	{
		TreeAccessScope access_scope(allocation_);
		int restartCount = 0;
restart:
		// need yield CPU when come here at second time
		if (restartCount++)
			yield(restartCount, true);
		bool needRestart = false;

		// Current node
		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || (node != load_root())) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;

		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);
			// Split eagerly if full
			if (inner->isFull()) {
				// Lock
				if (parent) {
					parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
					if (needRestart)
						goto restart;
				}
				node->upgradeToWriteLockOrRestart(versionNode, needRestart);
				if (needRestart) {
					if (parent)
						parent->writeUnlock();
					goto restart;
				}
				// parent is null and node isn't root
				if (!parent && (node != load_root())) {
					// there's a new parent
					node->writeUnlock();
					goto restart;
				}
				// Split
				KeyType sep;
				BTreeInner *newInner = inner->split(sep, allocation_);
				stats_.inner_nodes++;
				if (parent)
					parent->insert(sep, newInner, keyComp_);
				else
					makeRoot(sep, inner, newInner);

				// Unlock and restart
				node->writeUnlock();
				if (parent) {
					parent->writeUnlock();
				}
				goto restart;
			}

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			versionParent = versionNode;

			node = inner->childAt(inner->lowerBound(k, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			// prefetch((char *)node, sizeof(NodeMetaData));
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;

			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		} // while

		if (parent) {
			parent->checkOrRestart(versionParent, needRestart);
			if (needRestart) {
				node->readUnlockOrRestart(versionNode, needRestart);
				goto restart;
			}
		}
		auto leaf = static_cast<BTreeLeaf *>(node);
		ValueType insertRes;
		bool success;

		// Split leaf if full
		if (leaf->getCount() == leaf->maxEntries) {
			// Lock
			if (parent) {
				parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
				if (needRestart) {
					goto restart;
				}
			}
			node->upgradeToWriteLockOrRestart(versionNode, needRestart);
			if (needRestart) {
				if (parent)
					parent->writeUnlock();
				goto restart;
			}
			if (!parent && (node != load_root())) {
				// there's a new parent
				node->writeUnlock();
				goto restart;
			}
			// Split
			KeyType sep;
			BTreeLeaf *newLeaf = leaf->split(sep, allocation_);
			stats_.leaf_nodes++;
			if (keyComp_(k, sep) > 0) {
				success = newLeaf->insert(k, v, keyComp_);
			} else {
				success = leaf->insert(k, v, keyComp_);
			}

			if (success) {
				stats_.num_items++;
			}

			if (parent)
				parent->insert(sep, newLeaf, keyComp_);
			else
				makeRoot(sep, leaf, newLeaf);
			// Unlock and restart
			node->writeUnlock();
			if (parent)
				parent->writeUnlock();

			return success; // success
		} else {
			// only lock leaf node
			node->upgradeToWriteLockOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;
			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart) {
					node->writeUnlock();
					goto restart;
				}
			}

			success = leaf->insert(k, v, keyComp_);
			node->writeUnlock();
			// node->readUnlockOrRestart(versionParent, needRestart);
			if (success) {
				stats_.num_items++;
			}
			return success; // success
		}
	}

	// Mechanical offset-safe counterpart of BTreeOLC::insert_lock_next_key.
	// Keep the original narrower lock scope: unlike the adjacent-tuple variant,
	// this callback locks only the immediate successor leaf.
	bool insert_lock_next_key(
		const KeyType &k, const ValueType &v,
		std::function<bool(const KeyType *next_key, ValueType *next_value)>
			next_key_processor)
	{
		TreeAccessScope access_scope(allocation_);
		int restartCount = 0;
	restart:
		if (restartCount++) yield(restartCount, true);
		bool needRestart = false;
		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || node != load_root()) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;
		while (node->getType() == NodeType::BTreeInner) {
			auto *inner = static_cast<BTreeInner *>(node);
			if (inner->isFull()) {
				if (parent) {
					parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
					if (needRestart) goto restart;
				}
				node->upgradeToWriteLockOrRestart(versionNode, needRestart);
				if (needRestart) {
					if (parent) parent->writeUnlock();
					goto restart;
				}
				if (!parent && node != load_root()) {
					node->writeUnlock();
					goto restart;
				}
				KeyType sep;
				BTreeInner *newInner = inner->split(sep, allocation_);
				stats_.inner_nodes++;
				if (parent) parent->insert(sep, newInner, keyComp_);
				else makeRoot(sep, inner, newInner);
				node->writeUnlock();
				if (parent) parent->writeUnlock();
				goto restart;
			}
			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart) goto restart;
			}
			parent = inner;
			versionParent = versionNode;
			node = inner->childAt(inner->lowerBound(k, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart) goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart) goto restart;
		}
		if (parent) {
			parent->checkOrRestart(versionParent, needRestart);
			if (needRestart) {
				node->readUnlockOrRestart(versionNode, needRestart);
				goto restart;
			}
		}
		auto *leaf = static_cast<BTreeLeaf *>(node);
		if (leaf->getCount() == leaf->maxEntries) {
			if (parent) {
				parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
				if (needRestart) goto restart;
			}
			node->upgradeToWriteLockOrRestart(versionNode, needRestart);
			if (needRestart) {
				if (parent) parent->writeUnlock();
				goto restart;
			}
			if (!parent && node != load_root()) {
				node->writeUnlock();
				goto restart;
			}
			KeyType sep;
			BTreeLeaf *newLeaf = leaf->split(sep, allocation_);
			stats_.leaf_nodes++;
			if (parent) parent->insert(sep, newLeaf, keyComp_);
			else makeRoot(sep, leaf, newLeaf);
			node->writeUnlock();
			if (parent) parent->writeUnlock();
			goto restart;
		}
		node->upgradeToWriteLockOrRestart(versionNode, needRestart);
		if (needRestart) goto restart;
		if (parent) {
			parent->readUnlockOrRestart(versionParent, needRestart);
			if (needRestart) {
				node->writeUnlock();
				goto restart;
			}
		}
		const unsigned pos = leaf->lowerBound(k, keyComp_);
		bool success = false;
		BTreeLeaf *nextLeaf = nullptr;
		if (pos >= leaf->getCount() || keyComp_(leaf->keys_[pos], k) != 0) {
			const KeyType *next_key = nullptr;
			ValueType *next_value = nullptr;
			if (pos == leaf->getCount()) {
				nextLeaf = leaf->next_.get();
				CHECK(nextLeaf != nullptr);
				RecordTreeAccess(allocation_, nextLeaf, true);
				nextLeaf->writeLockOrRestart(needRestart);
				if (needRestart) {
					leaf->writeUnlock();
					goto restart;
				}
				next_key = &nextLeaf->keys_[0];
				next_value = &nextLeaf->values_[0];
			} else {
				next_key = &leaf->keys_[pos];
				next_value = &leaf->values_[pos];
			}
			if (next_key_processor(next_key, next_value)) {
				success = leaf->insert(k, v, keyComp_);
				CHECK(success == true);
			}
		}
		if (nextLeaf) nextLeaf->writeUnlock();
		node->writeUnlock();
		if (success) stats_.num_items++;
		return success;
	}

	// Mechanical offset-safe counterpart of BTreeOLC::
	// insert_and_process_adjacent_tuples.  The callback runs while the target
	// leaf and any leaf containing its immediate predecessor/successor remain
	// write-locked, exactly as in the original Tigon tree.
	bool insert_and_process_adjacent_tuples(
		const KeyType &k, const ValueType &v,
		std::function<bool(const KeyType *prev_key, ValueType *prev_value,
		                   const KeyType *next_key, ValueType *next_value)>
			adjacent_tuples_processor)
	{
		TreeAccessScope access_scope(allocation_);
		int restartCount = 0;
	restart:
		if (restartCount++)
			yield(restartCount, true);
		bool needRestart = false;

		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || node != load_root()) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;
		while (node->getType() == NodeType::BTreeInner) {
			auto *inner = static_cast<BTreeInner *>(node);
			if (inner->isFull()) {
				if (parent) {
					parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
					if (needRestart) goto restart;
				}
				node->upgradeToWriteLockOrRestart(versionNode, needRestart);
				if (needRestart) {
					if (parent) parent->writeUnlock();
					goto restart;
				}
				if (!parent && node != load_root()) {
					node->writeUnlock();
					goto restart;
				}
				KeyType sep;
				BTreeInner *newInner = inner->split(sep, allocation_);
				stats_.inner_nodes++;
				if (parent) parent->insert(sep, newInner, keyComp_);
				else makeRoot(sep, inner, newInner);
				node->writeUnlock();
				if (parent) parent->writeUnlock();
				goto restart;
			}

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart) goto restart;
			}
			parent = inner;
			versionParent = versionNode;
			node = inner->childAt(inner->lowerBound(k, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart) goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart) goto restart;
		}

		if (parent) {
			parent->checkOrRestart(versionParent, needRestart);
			if (needRestart) {
				node->readUnlockOrRestart(versionNode, needRestart);
				goto restart;
			}
		}
		auto *leaf = static_cast<BTreeLeaf *>(node);
		if (leaf->getCount() == leaf->maxEntries) {
			if (parent) {
				parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
				if (needRestart) goto restart;
			}
			node->upgradeToWriteLockOrRestart(versionNode, needRestart);
			if (needRestart) {
				if (parent) parent->writeUnlock();
				goto restart;
			}
			if (!parent && node != load_root()) {
				node->writeUnlock();
				goto restart;
			}
			KeyType sep;
			BTreeLeaf *newLeaf = leaf->split(sep, allocation_);
			stats_.leaf_nodes++;
			if (parent) parent->insert(sep, newLeaf, keyComp_);
			else makeRoot(sep, leaf, newLeaf);
			node->writeUnlock();
			if (parent) parent->writeUnlock();
			goto restart;
		}

		node->upgradeToWriteLockOrRestart(versionNode, needRestart);
		if (needRestart) goto restart;
		if (parent) {
			parent->readUnlockOrRestart(versionParent, needRestart);
			if (needRestart) {
				node->writeUnlock();
				goto restart;
			}
		}

		BTreeLeaf *prevLeaf = nullptr;
		BTreeLeaf *nextLeaf = nullptr;
		KeyType *prev_key = nullptr;
		KeyType *next_key = nullptr;
		ValueType *prev_value = nullptr;
		ValueType *next_value = nullptr;
		const unsigned pos = leaf->lowerBound(k, keyComp_);
		bool success = false;
		if (pos >= leaf->getCount() || keyComp_(leaf->keys_[pos], k) != 0) {
			if (pos > 0) {
				prev_key = &leaf->keys_[pos - 1];
				prev_value = &leaf->values_[pos - 1];
			} else if ((prevLeaf = leaf->pre_.get()) != nullptr) {
				RecordTreeAccess(allocation_, prevLeaf, true);
				prevLeaf->writeLockOrRestart(needRestart);
				if (needRestart) {
					leaf->writeUnlock();
					goto restart;
				}
				if (prevLeaf->getCount() > 0) {
					prev_key = &prevLeaf->keys_[prevLeaf->getCount() - 1];
					prev_value = &prevLeaf->values_[prevLeaf->getCount() - 1];
				}
			}
			if (pos == leaf->getCount()) {
				nextLeaf = leaf->next_.get();
				CHECK(nextLeaf != nullptr);
				RecordTreeAccess(allocation_, nextLeaf, true);
				nextLeaf->writeLockOrRestart(needRestart);
				if (needRestart) {
					leaf->writeUnlock();
					if (prevLeaf) prevLeaf->writeUnlock();
					goto restart;
				}
				next_key = &nextLeaf->keys_[0];
				next_value = &nextLeaf->values_[0];
			} else {
				next_key = &leaf->keys_[pos];
				next_value = &leaf->values_[pos];
			}
			if (adjacent_tuples_processor(prev_key, prev_value, next_key, next_value)) {
				success = leaf->insert(k, v, keyComp_);
				CHECK(success == true);
			}
		}
		if (prevLeaf) prevLeaf->writeUnlock();
		if (nextLeaf) nextLeaf->writeUnlock();
		node->writeUnlock();
		if (success) stats_.num_items++;
		return success;
	}

	/**
	 * return v if insert successful
	 * otherwise return an existing value
	 */
	ValueType insert(const KeyType &k, const ValueType &v, bool *result)
	{
		TreeAccessScope access_scope(allocation_);
		int restartCount = 0;
restart:
		// need yield CPU when come here at second time
		if (restartCount++)
			yield(restartCount, true);
		bool needRestart = false;

		// Current node
		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || (node != load_root())) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;

		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);
			// Split eagerly if full
			if (inner->isFull()) {
				// Lock
				if (parent) {
					parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
					if (needRestart)
						goto restart;
				}
				node->upgradeToWriteLockOrRestart(versionNode, needRestart);
				if (needRestart) {
					if (parent)
						parent->writeUnlock();
					goto restart;
				}
				// parent is null and node isn't root
				if (!parent && (node != load_root())) {
					// there's a new parent
					node->writeUnlock();
					goto restart;
				}
				// Split
				KeyType sep;
				BTreeInner *newInner = inner->split(sep, allocation_);
				stats_.inner_nodes++;
				if (parent)
					parent->insert(sep, newInner, keyComp_);
				else
					makeRoot(sep, inner, newInner);

				if (parent) {
					parent->downgradeToReadLock(versionParent);
				}

				if (keyComp_(k, sep) > 0) {
					inner->writeUnlock();
					inner = newInner;
					versionNode = newInner->readLockOrRestart(needRestart);
					if (needRestart)
						goto restart;
				} else {
					node->downgradeToReadLock(versionNode);
				}
			}

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			versionParent = versionNode;

			node = inner->childAt(inner->lowerBound(k, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			// prefetch((char *)node, sizeof(NodeMetaData));
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;

			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		} // while

		auto leaf = static_cast<BTreeLeaf *>(node);
		ValueType insertRes;
		bool success;

		// Split leaf if full
		if (leaf->getCount() == leaf->maxEntries) {
			// Lock
			if (parent) {
				parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
				if (needRestart) {
					node->readUnlockOrRestart(versionNode, needRestart);
					goto restart;
				}
			}
			node->upgradeToWriteLockOrRestart(versionNode, needRestart);
			if (needRestart) {
				node->readUnlockOrRestart(versionNode, needRestart);
				if (parent)
					parent->writeUnlock();
				goto restart;
			}
			if (!parent && (node != load_root())) {
				// there's a new parent
				node->writeUnlock();
				goto restart;
			}
			// Split
			KeyType sep;
			BTreeLeaf *newLeaf = leaf->split(sep, allocation_);
			stats_.leaf_nodes++;
			if (keyComp_(k, sep) > 0) {
				insertRes = newLeaf->insert(k, v, keyComp_, success);
			} else {
				insertRes = leaf->insert(k, v, keyComp_, success);
			}

			if (parent)
				parent->insert(sep, newLeaf, keyComp_);
			else
				makeRoot(sep, leaf, newLeaf);
			// Unlock and restart
			node->writeUnlock();
			if (parent)
				parent->writeUnlock();

			if (success) {
				stats_.num_items++;
			}
			if (result)
				*result = success;
			return insertRes; // success
		} else {
			// only lock leaf node
			node->upgradeToWriteLockOrRestart(versionNode, needRestart);
			if (needRestart) {
				node->readUnlockOrRestart(versionNode, needRestart);
				goto restart;
			}
			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart) {
					node->writeUnlock();
					goto restart;
				}
			}
			insertRes = leaf->insert(k, v, keyComp_, success);
			node->writeUnlock();
			// node->readUnlockOrRestart(versionParent, needRestart);

			if (result)
				*result = success;
			if (success) {
				stats_.num_items++;
			}
			return insertRes; // success
		}
	}

	/**
	 * return v if the <key, value> already exists
	 * otherwise, create a <k, v> and return v
	 */
	ValueType getValue(const KeyType &k, std::function<ValueType(void)> createValue)
	{
		int restartCount = 0;
restart:
		// need yield CPU when come here at second time
		if (restartCount++)
			yield(restartCount, true);
		bool needRestart = false;

		// Current node
		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || (node != load_root())) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;

		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);
			// Split eagerly if full
			if (inner->isFull()) {
				// Lock
				if (parent) {
					parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
					if (needRestart)
						goto restart;
				}
				node->upgradeToWriteLockOrRestart(versionNode, needRestart);
				if (needRestart) {
					if (parent)
						parent->writeUnlock();
					goto restart;
				}
				// parent is null and node isn't root
				if (!parent && (node != load_root())) {
					// there's a new parent
					node->writeUnlock();
					goto restart;
				}
				// Split
				KeyType sep;
				BTreeInner *newInner = inner->split(sep, allocation_);
				stats_.inner_nodes++;
				if (parent)
					parent->insert(sep, newInner, keyComp_);
				else
					makeRoot(sep, inner, newInner);

				if (parent) {
					parent->downgradeToReadLock(versionParent);
				}

				if (keyComp_(k, sep) > 0) {
					inner->writeUnlock();
					inner = newInner;
					versionNode = newInner->readLockOrRestart(needRestart);
					if (needRestart)
						goto restart;
				} else {
					node->downgradeToReadLock(versionNode);
				}
			}

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			versionParent = versionNode;

			node = inner->childAt(inner->lowerBound(k, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			// prefetch((char *)node, sizeof(NodeMetaData));
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;

			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		} // while

		auto leaf = static_cast<BTreeLeaf *>(node);
		ValueType insertRes;
		bool success;

		// Split leaf if full
		if (leaf->getCount() == leaf->maxEntries) {
			// Lock
			if (parent) {
				parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
				if (needRestart) {
					node->readUnlockOrRestart(versionNode, needRestart);
					goto restart;
				}
			}
			node->upgradeToWriteLockOrRestart(versionNode, needRestart);
			if (needRestart) {
				node->readUnlockOrRestart(versionNode, needRestart);
				if (parent) {
					parent->writeUnlock();
				}
				goto restart;
			}
			if (!parent && (node != load_root())) {
				// there's a new parent
				node->writeUnlock();
				goto restart;
			}
			// Split
			KeyType sep;
			BTreeLeaf *newLeaf = leaf->split(sep, allocation_);
			stats_.leaf_nodes++;
			if (keyComp_(k, sep) > 0) {
				insertRes = newLeaf->getValue(k, keyComp_, createValue, success);
			} else {
				insertRes = leaf->getValue(k, keyComp_, createValue, success);
			}

			if (success) {
				stats_.num_items++;
			}
			if (parent)
				parent->insert(sep, newLeaf, keyComp_);
			else
				makeRoot(sep, leaf, newLeaf);
			// Unlock and restart
			node->writeUnlock();
			if (parent)
				parent->writeUnlock();
			return insertRes; // success
		} else {
			// only lock leaf node
			node->upgradeToWriteLockOrRestart(versionNode, needRestart);
			if (needRestart) {
				node->readUnlockOrRestart(versionNode, needRestart);
				goto restart;
			}
			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart) {
					node->writeUnlock();
					goto restart;
				}
			}
			insertRes = leaf->getValue(k, keyComp_, createValue, success);
			if (success) {
				stats_.num_items++;
			}
			node->writeUnlock();
			return insertRes; // success
		}
	}

	/**
	 * Delete key and its corresponding value if only if the value satisfies the predicate
	 * return RemoveResult::GOOD if key exists and delete successfully
	 *        RemoveResult::KEY_NOT_FOUND if key does not exists
	 *        RemoveResult::VALUE_NOT_SATISFYING_PREDICATE if the value does not pass value_predicate check
	 */
	btreeolc_cxl::RemoveResult remove(const KeyType &key, std::function<btreeolc_cxl::RemovePredicateResult(const ValueType &)> value_predicate)
	{
		TreeAccessScope access_scope(allocation_);
		ValueType v;
		return _remove_with_value_predicate(std::make_pair(key, v), value_predicate);
	}

	/**
	 * Delete key and its corresponding value
	 * return true if key exists and delete successfully
	 */
	bool remove(const KeyType &key)
	{
		TreeAccessScope access_scope(allocation_);
		return _remove(key);
	}

	/**
	 * Delete <key, value>
	 * return true if <key, value> exists and delete successfully
	 */
	bool remove(const KeyType &key, ValueType value)
	{
		TreeAccessScope access_scope(allocation_);
		return _remove(key);
	}

	/**
	 * Delete key and process its adjacent keys atomically.
	 * The callback contract and leaf-latch timing are the original BTreeOLC
	 * contract; this CXL variant only records the HWCC node accesses.
	 */
	bool remove_and_process_adjacent_keys(
		const KeyType &key,
		std::function<bool(const KeyType *prev_key, ValueType *prev_value,
					   const KeyType *cur_key, ValueType *cur_value,
					   const KeyType *next_key, ValueType *next_value)>
			adjacent_tuples_processor)
	{
		TreeAccessScope access_scope(allocation_);
		return _remove_and_process_adjacent_tuples(key, adjacent_tuples_processor);
	}

	/**
	 * @param leftExist: Left interval is `[` when leftExist is true, otherwise '('
	 *                   Invalid when leftExist is false
	 * @param rightExist: Right interval is `]` when rightExist is true, otherwise ')'
	 *                    Invalid when rightExist is false
	 * @param limit: Upper bound for the number of elements
	 *               unlimited if limit is 0
	 *
	 * @return: return true if need to retry
	 *
	 * NOTE: res will be clear
	 */
	void scan(const KeyType &lowKey, const KeyType &highKey, bool leftExist, bool rightExist, uint32_t limit, std::vector<KeyValuePair> &res)
	{
		TreeAccessScope access_scope(allocation_);
		int restartCount = 0;
restart:
		res.clear();
		if (restartCount++)
			yield(restartCount);
		bool needRestart = false;

		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || (node != load_root())) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent;

		// find the first leafNode
		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			versionParent = versionNode;
			node = inner->childAt(inner->lowerBound(lowKey, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);

			// prefetch((char *)node, kPageSize);
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		}
		node->checkOrRestart(versionNode, needRestart);
		if (needRestart)
			goto restart;
		auto leaf = static_cast<BTreeLeaf *>(node);
		unsigned pos = leaf->lowerBound(lowKey, keyComp_);
		if (leaf == nullptr)
			return;
		// Adjust the leaf node
		// according to the `leftExist`
		if (!leftExist && keyComp_(lowKey, leaf->keys_[pos]) == 0) {
			if ((int)pos == leaf->getCount() - 1) {
				auto *nextLeaf = leaf->next_.get();
				// Validate before following next_ (OLC pointer-then-check).
				leaf->checkOrRestart(versionNode, needRestart);
				if (needRestart)
					goto restart;
				leaf = nextLeaf;
				RecordTreeAccess(allocation_, leaf, false);
				pos = 0;
			} else {
				pos++;
			}
		}
		if (leaf == nullptr)
			return;

		leaf->iteratorEnter(needRestart);
		{
			BPlusTreeIterator itr(leaf, pos, &allocation_);
			if (itr == retryItr())
				goto restart;

			while ((limit == 0 || res.size() < limit) && (itr != endItr()) &&
			       ((!rightExist && keyComp_(itr.key(), highKey) < 0) || (rightExist && keyComp_(itr.key(), highKey) <= 0))) {
				res.push_back({ itr.key(), itr.value() });
				itr++;
				// needs scan again
				if (itr == retryItr())
					goto restart;
			}
		}
	}

	/**
	 * Range scan items starting at `startKey` for update.
	 * Leaves will be write-locked.
	 * The scan ends when processor returns true or all items are traversed.
	 */
	void scanForUpdate(const KeyType &startKey, std::function<bool(const KeyType &, ValueType &, bool)> processor)
	{
		TreeAccessScope access_scope(allocation_);
		bool leftExist = true;
		int restartCount = 0;
		int leavesTraversed = 0;
		KeyType lowKey = startKey;
restart:
		if (restartCount++)
			yield(restartCount);
		bool needRestart = false;

		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || (node != load_root())) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent;

		// find the first leafNode
		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			versionParent = versionNode;
			node = inner->childAt(inner->lowerBound(lowKey, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);

			// prefetch((char *)node, kPageSize);
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		}
		auto leaf = static_cast<BTreeLeaf *>(node);

		node->upgradeToWriteLockOrRestart(versionNode, needRestart);
		if (needRestart) {
			if (leavesTraversed == 0) {
				lowKey = startKey;
			}
			goto restart;
		}
		if (parent) {
			parent->readUnlockOrRestart(versionParent, needRestart);
			if (needRestart) {
				node->writeUnlock();
				goto restart;
			}
		}

		unsigned pos = leaf->lowerBound(lowKey, keyComp_);

		bool quit = false;
		BTreeLeaf *nextLeaf = leaf->next_.get();
		for (unsigned p = pos; p < leaf->getCount(); ++p) {
			bool lastItem = nextLeaf == nullptr && p + 1 == leaf->getCount();
			RecordTreeDataRead(&leaf->keys_[p], sizeof(KeyType));
			RecordTreeDataRead(&leaf->values_[p], sizeof(ValueType));
                        CHECK(keyComp_(leaf->keys_[p], lowKey) >= 0);
			bool end = processor(leaf->keys_[p], leaf->values_[p], lastItem);
			if (end) {
				quit = true;
				break;
			}
		}
		node->writeUnlock();
		leavesTraversed++;
		if (quit == false && nextLeaf != nullptr) {
			// versionNode = nextLeaf->readLockOrRestart(needRestart);
			if (nextLeaf->getCount() > 0) {
				lowKey = nextLeaf->keys_[0];
			} else {
				quit = true;
			}
			// nextLeaf->readUnlockOrRestart(versionNode, needRestart);
			// goto restart;
		} else {
			quit = true;
		}

		if (quit == false) {
			goto restart;
		}
	}

	// Keep the master TableBTreeOLC entrypoint available for offset-backed
	// tables.  The CXL tree's update traversal already owns the required leaf
	// latch and access accounting, so this compatibility entrypoint preserves
	// that single traversal implementation.
	void scanForUpdateNoContention(const KeyType &startKey,
	                               std::function<bool(const KeyType &, ValueType &, bool)> processor)
	{
		scanForUpdate(startKey, std::move(processor));
	}

	/**
	 * Assume the nodes in stack are all optimistically read-locked,
	 * meaning the version numbers are stored in the stack.
	 * 1. If there is a sibling doesn't meet the "half full" criteria,
	 * try to borrow a KeyType from the sibling.
	 * 2. Otherwise, try to merge a sibling, left sibling first.
	 */
	bool EraseMerge(std::vector<StackNodeElement> &stack)
	{
		assert(stack.empty() == false);
		if (stack.size() == 1) {
			// auto e = stack.back();
			// bool needRestart = true;
			// e.node->readUnlockOrRestart(e.version, needRestart);
			return false;
		}
		// the last element of stack is a leaf Node
		StackNodeElement tope = stack[stack.size() - 1];
		NodeBase *childNode = tope.node.get();
		uint64_t childVersion = tope.version;
		unsigned childPos = tope.pos;
		{
			bool restart = false;
			childNode->checkOrRestart(childVersion, restart);
			if (restart) {
				return false;
			}
		}
		/*           R[1]
		      A[1]           B[0]
		  A1[2] A2[2]      B1[1]
		*/
		assert(stack.size() >= 2);
		StackNodeElement parente = stack[stack.size() - 2];
		// parent node must be inner node
		BTreeInner *parentNode = static_cast<BTreeInner *>(parente.node.get());
		uint64_t parentVersion = parente.version;
		// unsigned parentPos = parente.pos;

		uint64_t siblingVersion;
		enum class MergeOperation {
			BorrowFromLeft,
			BorrowFromRight,
			LeftToRight,
			RightToLeft, // put right on left
			NoMerge, // no operation
		};

		MergeOperation opt = MergeOperation::NoMerge;
		// the position of left sibling or right sibling
		int siblingPos = -1;
		// If current node is the only node stored in parent
		bool single_child = false;
		auto choose_one_sibling = [&]() -> NodeBase * {
			unsigned leftSiblingPos = childPos - 1;
			unsigned rightSiblingPos = childPos + 1;
			NodeBase *leftSibling = nullptr, *rightSibling = nullptr;
			if (leftSiblingPos >= 0 && leftSiblingPos < parentNode->getCount()) {
				leftSibling = parentNode->childAt(leftSiblingPos).get();
			}
			if (rightSiblingPos > 0 && rightSiblingPos <= parentNode->getCount()) {
				rightSibling = parentNode->childAt(rightSiblingPos).get();
			}
			if (leftSibling == nullptr && rightSibling == nullptr) {
				single_child = true;
			}
			// try borrow from left sibling
			if (leftSibling) {
				bool restart = false;
				siblingVersion = leftSibling->readLockOrRestart(restart);
				if (restart == false && getSurplus(leftSibling) > 0) {
					siblingPos = leftSiblingPos;
					opt = MergeOperation::BorrowFromLeft;
					return leftSibling;
				}
			}
			// try borrow from right sibling
			if (rightSibling) {
				bool restart = false;
				siblingVersion = rightSibling->readLockOrRestart(restart);
				if (restart == false && getSurplus(rightSibling) > 0) {
					siblingPos = rightSiblingPos;
					opt = MergeOperation::BorrowFromRight;
					return rightSibling;
				}
			}

			if (leftSibling) {
				bool restart = false;
				// get the read lock
				siblingVersion = leftSibling->readLockOrRestart(restart);
				// if restart is true, try right sibling
				if (restart == false) {
					if (hasEnoughSpace(leftSibling, childNode->getCount())) {
						siblingPos = leftSiblingPos;
						opt = MergeOperation::RightToLeft;
						return leftSibling;
					}
				}
				// release the read lock of left sibling
			}

			if (rightSibling) {
				bool restart = false;
				// get the read lock
				siblingVersion = rightSibling->readLockOrRestart(restart);
				if (restart == false) {
					if (hasEnoughSpace(rightSibling, childNode->getCount())) {
						siblingPos = rightSiblingPos;
						opt = MergeOperation::LeftToRight;
						return rightSibling;
					}
				}
			}
			return nullptr;
		};

		bool restart = false;
		NodeBase *siblingNode = choose_one_sibling();
		if (siblingNode == nullptr) {
			// if childNode is the only child of parent, try merge the parent node first so as to get more "siblings".
			if (single_child && stack.size() > 2) {
				stack.pop_back();
				EraseMerge(stack);
			}
			return false; // retry
		}

		// Lock protocol, top to bottom, left to right

		parentNode->upgradeToWriteLockOrRestart(parentVersion, restart);
		if (restart) {
			return false;
		}

		if (opt == MergeOperation::LeftToRight || opt == MergeOperation::BorrowFromRight) {
			childNode->upgradeToWriteLockOrRestart(childVersion, restart);
			if (restart) {
				parentNode->writeUnlock();
				return false;
			}
			siblingNode->upgradeToWriteLockOrRestart(siblingVersion, restart);
			if (restart) {
				childNode->writeUnlock();
				parentNode->writeUnlock();
				return false;
			}
		} else if (opt == MergeOperation::RightToLeft || opt == MergeOperation::BorrowFromLeft) {
			siblingNode->upgradeToWriteLockOrRestart(siblingVersion, restart);
			if (restart) {
				parentNode->writeUnlock();
				return false;
			}
			childNode->upgradeToWriteLockOrRestart(childVersion, restart);
			if (restart) {
				siblingNode->writeUnlock();
				parentNode->writeUnlock();
				return false;
			}
		}

		bool parentNeedMerge = false, changeRoot = false;
		if (childNode->getType() == NodeType::BTreeLeaf) {
			auto child = static_cast<BTreeLeaf *>(childNode);
			auto sibling = static_cast<BTreeLeaf *>(siblingNode);
			if (opt == MergeOperation::LeftToRight) {
				child->merge(sibling, allocation_);
				stats_.leaf_nodes--;
				// KeyType siblingMaxKey = sibling->max_key();
				// parentNode->keys_[childPos] = siblingMaxKey;
				changeRoot = parentNode->erase(childPos, allocation_);
				// if (changeRoot && parentNode == load_root()) {
				if (changeRoot && parentNode == load_root()) {
					assert(((uint64_t)parentNode) != 0xffffffffffffffffull);
					allocation_.Retire(parentNode, kLeafPageSize);
					store_root(child);
					// if (parentNode == load_root()) store_root(child);
				}
			} else if (opt == MergeOperation::RightToLeft) {
				sibling->merge(child, allocation_);
				stats_.leaf_nodes--;
				// KeyType childMaxKey = child->max_key();
				// parentNode->keys_[siblingPos] = childMaxKey;
				changeRoot = parentNode->erase(siblingPos, allocation_);
				if (changeRoot && parentNode == load_root()) {
					// if (changeRoot) {
					assert(((uint64_t)parentNode) != 0xffffffffffffffffull);
					allocation_.Retire(parentNode, kLeafPageSize);
					store_root(sibling);
				}
			} else if (opt == MergeOperation::BorrowFromRight) {
				reallocNode(childNode, siblingNode, 1, parentNode, childPos);
			} else if (opt == MergeOperation::BorrowFromLeft) {
				reallocNode(siblingNode, childNode, 0, parentNode, siblingPos);
			}
		} else {
			auto child = static_cast<BTreeInner *>(childNode);
			auto sibling = static_cast<BTreeInner *>(siblingNode);
			if (opt == MergeOperation::LeftToRight) {
				const KeyType &subTreeMaxKey = _getSubTreeMaxKey(child->childAt(child->getCount()).get());
				child->merge(sibling, subTreeMaxKey, allocation_);
				stats_.inner_nodes--;
				// KeyType siblingSubTreeMaxKey = _getSubTreeMaxKey(sibling);
				changeRoot = parentNode->erase(childPos, allocation_);
				if (changeRoot && parentNode == load_root()) {
					// if (changeRoot) {
					assert(((uint64_t)parentNode) != 0xffffffffffffffffull);
					allocation_.Retire(parentNode, kLeafPageSize);
					store_root(child);
				}
			} else if (opt == MergeOperation::RightToLeft) {
				const KeyType &subTreeMaxKey = _getSubTreeMaxKey(sibling->childAt(sibling->getCount()).get());
				sibling->merge(child, subTreeMaxKey, allocation_);
				stats_.inner_nodes--;
				changeRoot = parentNode->erase(siblingPos, allocation_);
				if (changeRoot && parentNode == load_root()) {
					// if (changeRoot) {
					assert(((uint64_t)parentNode) != 0xffffffffffffffffull);
					allocation_.Retire(parentNode, kLeafPageSize);
					store_root(sibling);
				}
			} else if (opt == MergeOperation::BorrowFromRight) {
				reallocNode(childNode, siblingNode, 1, parentNode, childPos);
			} else if (opt == MergeOperation::BorrowFromLeft) {
				reallocNode(siblingNode, childNode, 0, parentNode, siblingPos);
			}
		}

		if (opt != MergeOperation::BorrowFromRight && opt != MergeOperation::BorrowFromLeft) {
			parentNeedMerge = parentNode->needMerge();
		}

		if (opt == MergeOperation::LeftToRight || opt == MergeOperation::BorrowFromRight) {
			siblingNode->writeUnlock();
			childNode->writeUnlock();
		} else if (opt == MergeOperation::RightToLeft || opt == MergeOperation::BorrowFromLeft) {
			childNode->writeUnlock();
			siblingNode->writeUnlock();
		}

		parentNode->writeUnlock();
		if (parentNeedMerge) {
			stack.pop_back();
			parentVersion = parentNode->readLockOrRestart(restart);
			stack.back().version = parentVersion;
			EraseMerge(stack);
		}
		return true;
	}

	/**
	 * find key and all its corresponding values
	 * return true if key exists
	 */
	bool lookup(const KeyType &key, ValueType &result)
	{
		TreeAccessScope access_scope(allocation_);
		return _lookup(key, result);
	}

	/**
	 * find key and all its corresponding values
	 * return true if key exists
	 */
	bool lookupForUpdate(const KeyType &key, std::function<void(const KeyType &key, ValueType &value)> update_processor)
	{
		TreeAccessScope access_scope(allocation_);
		return _lookupForUpdate(key, update_processor);
	}

	// Original lookupForNextKeyUpdate primitive, retained for the table
	// adapter's next-key metadata update path.
	bool lookupForNextKeyUpdate(
		const KeyType &key,
		std::function<void(const KeyType *, ValueType *, const KeyType *,
					   ValueType *, const KeyType *, ValueType *)> processor)
	{
		TreeAccessScope access_scope(allocation_);
		return _lookupForNextKeyUpdate(key, processor);
	}

	/**
	 * find <key, value>
	 * return true if <key, value> exists and delete successfully
	 * NOTE: lookup only append data to result
	 */
	bool lookup(const KeyType &key, const ValueType &value, ValueType &result)
	{
		TreeAccessScope access_scope(allocation_);
		return _lookup({ key, value }, result, false);
	}

	/**
	 * Returns retryItr() iterator for the B+ Tree
	 */
	BPlusTreeIterator retryItr()
	{
		return BPlusTreeIterator::getRetryIterator();
	}

	/**
	 * Returns endItr() iterator for the B+ Tree
	 */
	BPlusTreeIterator endItr()
	{
		return BPlusTreeIterator::getEndIterator();
	}

	uint64_t getInnerNodeSize() const
	{
		return InnerPageSize;
	}

	uint64_t getLeafNodeSize() const
	{
		return sizeof(BTreeLeaf);
	}

	uint64_t getNumInnerNodes() const
	{
		return stats_.inner_nodes.load();
	}

	uint64_t getNumLeafNodes() const
	{
		return stats_.leaf_nodes.load();
	}

	std::size_t size() const
	{
		return stats_.num_items;
	}

	double getAverageFillFactor() const
	{
		auto numLeaves = getNumLeafNodes();
		auto maximumItems = numLeaves * BTreeLeaf::maxEntries;
		return stats_.num_items / (maximumItems * 1.0);
	}

    private:
	NodeBase *load_root() const
	{
		if (published_root_ != nullptr) {
			// The published root slot follows the tree domain: shared trees
			// publish into the HWCC layout, private trees into their
			// owner-private SWCC arena.
			const auto off =
			    TreeAtomicLoad(*published_root_, std::memory_order_acquire);
			if (off == tigonkv::engine::kNullOffset) return nullptr;
			return static_cast<NodeBase *>(allocation_.FromOffset(off));
		}
		const auto off = root_.load(std::memory_order_acquire);
		return off == tigonkv::engine::kNullOffset
		           ? nullptr
		           : static_cast<NodeBase *>(allocation_.FromOffset(off));
	}

	void store_root(NodeBase *node)
	{
		root_.store(node == nullptr ? tigonkv::engine::kNullOffset
		                            : allocation_.ToOffset(node),
		            std::memory_order_release);
		if (published_root_ != nullptr) {
			TreeAtomicStore(*published_root_, allocation_.ToOffset(node),
			                std::memory_order_release);
		}
	}

	const KeyComparator keyComp_;
	const ValueComparator valueComp_;

	/** whether key is unique */
	const int keyUnique_;
	const TreeNodeAllocation allocation_;

	std::atomic<tigonkv::engine::RegionOffset> root_{tigonkv::engine::kNullOffset};
	// When non-null (shared CXL trees), live root truth is this HWCC slot.
	std::atomic<tigonkv::engine::RegionOffset> *published_root_{ nullptr };

	struct tree_stats {
		std::atomic<uint64_t> inner_nodes{ 0 };
		char pad1_[64];
		std::atomic<uint64_t> leaf_nodes{ 0 };
		char pad2_[64];
		std::atomic<uint64_t> num_items{ 0 };
	};
	char pad_[64];
	tree_stats stats_;

	const KeyType &_getSubTreeMaxKey(NodeBase *node)
	{
		int restartCount = 0;
restart:
		if (restartCount++)
			yield(restartCount);

		bool needRestart = false;
		RecordTreeAccess(allocation_, node, false);
		uint64_t version = node->readLockOrRestart(needRestart);
		if (needRestart)
			goto restart;

		BTreeInner *parent = nullptr;
		uint64_t parentVersion = 0;

		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);
			if (parent) {
				parent->readUnlockOrRestart(parentVersion, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			parentVersion = version;

			// get the last child
			auto pos = node->getCount();
			node = inner->childAt(pos).get();
			RecordTreeAccess(allocation_, node, false);

			inner->checkOrRestart(version, needRestart);
			if (needRestart)
				goto restart;
			version = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		}

		auto leaf = static_cast<BTreeLeaf *>(node);
		// int pos = leaf->getCount() ? leaf->getCount() - 1 : 0;
		const KeyType &res = leaf->max_key();
		if (parent) {
			parent->readUnlockOrRestart(parentVersion, needRestart);
			if (needRestart) {
				leaf->readUnlockOrRestart(version, needRestart);
				goto restart;
			}
		}

		node->readUnlockOrRestart(version, needRestart);
		if (needRestart)
			goto restart;
		return res;
	}

	btreeolc_cxl::RemoveResult _remove_with_value_predicate(const KeyValuePair &element,
							    std::function<btreeolc_cxl::RemovePredicateResult(const ValueType &)> predicate)
	{
		int restartCount = 0;
		btreeolc_cxl::RemoveResult saved_result = btreeolc_cxl::RemoveResult::VALUE_NOT_SATISFYING_PREDICATE;
		bool result_saved = false;
restart:
		if (restartCount++)
			yield(restartCount, true);
		bool needRestart = false;
		// stores the path from root to leaf.
		// each element stores the node and the position in the parent node
		// from which the current node is derived.
		std::vector<StackNodeElement> stack;
		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || node != load_root()) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}
		stack.push_back(StackNodeElement{ node, -1, versionNode });

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;

		while (node->getType() == NodeType::BTreeInner) {
			BTreeInner *inner = static_cast<BTreeInner *>(node);
			if (parent) {
				// check the version only, don't call `readUnlockOrRestart`
				parent->checkOrRestart(versionParent, needRestart);
				if (needRestart) {
					goto restart;
				}
			}
			// inner -> parent, inner -> parent.child
			parent = inner;
			versionParent = versionNode;

			unsigned pos = inner->lowerBound(element.first, keyComp_);
			node = inner->childAt(pos).get();
			RecordTreeAccess(allocation_, node, false);
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart) {
				goto restart;
			}
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart) {
				goto restart;
			}

			stack.push_back(StackNodeElement{ node, int(pos), versionNode });
		}

		if (parent) {
			// check the version only, don't call `readUnlockOrRestart`
			parent->checkOrRestart(versionParent, needRestart);
			if (needRestart) {
				node->readUnlockOrRestart(versionNode, needRestart);
				assert(needRestart == false);
				goto restart;
			}
		}
		// reach the leaf node
		BTreeLeaf *leafNode = static_cast<BTreeLeaf *>(node);
		unsigned pos = leafNode->lowerBound(element.first, keyComp_);
		btreeolc_cxl::RemoveResult result = btreeolc_cxl::RemoveResult::GOOD;
		bool leafNeedMerge = false;
		if (pos < leafNode->getCount() && result_saved == false) {
			const KeyType &key = leafNode->keys_[pos];
			const ValueType &value = leafNode->values_[pos];
			// const KeyValuePair & kv = ;
			//  the leaf node contains the key
			if (keyComp_(key, element.first) == 0) {
				leafNode->upgradeToWriteLockOrRestart(versionNode, needRestart);
				if (needRestart) {
					leafNode->readUnlockOrRestart(versionNode, needRestart);
					goto restart;
				}
				if (parent) {
					// check the version only, don't call `readUnlockOrRestart`
					parent->checkOrRestart(versionParent, needRestart);
					if (needRestart) {
						leafNode->writeUnlock();
						goto restart;
					}
				}
				auto predicate_result = predicate(value);

				if (predicate_result == btreeolc_cxl::RemovePredicateResult::GOOD) {
					leafNode->erase(pos, keyComp_, valueComp_, false);
					result = btreeolc_cxl::RemoveResult::GOOD;
					stats_.num_items--;
				} else if (predicate_result == btreeolc_cxl::RemovePredicateResult::VALUE_HAS_OTHER_REFERENCE) {
					result = btreeolc_cxl::RemoveResult::VALUE_HAS_OTHER_REFERENCE;
				} else {
					result = btreeolc_cxl::RemoveResult::VALUE_NOT_SATISFYING_PREDICATE;
				}

				leafNode->downgradeToReadLock(versionNode);

				result_saved = true;
				saved_result = result;

				leafNeedMerge = leafNode->needMerge();

				// No merge if leaf is the root.
				if (stack.size() > 1 && leafNeedMerge) {
					stack.back().version = versionNode;
					auto stack_copy = stack;
					if (!EraseMerge(stack_copy)) {
						leafNode->readUnlockOrRestart(versionNode, needRestart);
						// The leaf is under-utilize and EraseMerge failed due to conflicts, retry until it succeeds.
						// We save the result of delete operation to avoid deleting a key twice.
						goto restart;
					}
				} else {
					leafNode->readUnlockOrRestart(versionNode, needRestart);
				}
				return result;
			} else {
				result = btreeolc_cxl::RemoveResult::KEY_NOT_FOUND;
			}
			result_saved = true;
			saved_result = result;
		} else {
			result = btreeolc_cxl::RemoveResult::KEY_NOT_FOUND;
		}

		if (result_saved == false) {
			result_saved = true;
			saved_result = result;
		}

		// Check if the leaf is under-utilized.
		leafNeedMerge = leafNode->needMerge();
		// No merge if leaf is the root.
		if (stack.size() > 1 && leafNeedMerge) {
			stack.back().version = versionNode;
			auto stack_copy = stack;
			if (!EraseMerge(stack_copy)) {
				leafNode->readUnlockOrRestart(versionNode, needRestart);
				// The leaf is under-utilize and EraseMerge failed due to conflict, retry until it succeeds.
				goto restart;
			}
		} else {
			leafNode->readUnlockOrRestart(versionNode, needRestart);
		}

		assert(result_saved);
		return saved_result;
	}

	/**
	 * @param flag Delete key and all its corresponding values when flag is true
	 */
	bool _remove(const KeyType &deleteKey)
	{
		int restartCount = 0;
		bool saved_success = false;
		bool result_saved = false;
restart:
		if (restartCount++)
			yield(restartCount, true);
		bool needRestart = false;
		// stores the path from root to leaf.
		// each element stores the node and the position in the parent node
		// from which the current node is derived.
		std::vector<StackNodeElement> stack;
		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || node != load_root()) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}
		stack.push_back(StackNodeElement{ node, -1, versionNode });

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;

		while (node->getType() == NodeType::BTreeInner) {
			BTreeInner *inner = static_cast<BTreeInner *>(node);
			if (parent) {
				// check the version only, don't call `readUnlockOrRestart`
				parent->checkOrRestart(versionParent, needRestart);
				if (needRestart) {
					goto restart;
				}
			}
			// inner -> parent, inner -> parent.child
			parent = inner;
			versionParent = versionNode;

			unsigned pos = inner->lowerBound(deleteKey, keyComp_);
			node = inner->childAt(pos).get();
			RecordTreeAccess(allocation_, node, false);
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart) {
				goto restart;
			}
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart) {
				goto restart;
			}

			stack.push_back(StackNodeElement{ node, int(pos), versionNode });
		}

		// reach the leaf node
		BTreeLeaf *leafNode = static_cast<BTreeLeaf *>(node);
		leafNode->upgradeToWriteLockOrRestart(versionNode, needRestart);
		if (needRestart) {
			goto restart;
		}
		if (parent) {
			// check the version only, don't call `readUnlockOrRestart`
			parent->checkOrRestart(versionParent, needRestart);
			if (needRestart) {
				leafNode->writeUnlock();
				goto restart;
			}
		}

		unsigned pos = leafNode->lowerBound(deleteKey, keyComp_);
		bool success = false, leafNeedMerge = false;
		if (pos < leafNode->getCount() && saved_success == false) {
			// const KeyValuePair &kv = leafNode->data_[pos];
			const KeyType &key = leafNode->keys_[pos];
			// const ValueType & value = leafNode->values_[pos];
			//  the leaf node contains the key
			if (keyComp_(key, deleteKey) == 0) {
				assert(keyComp_(leafNode->keys_[pos], deleteKey) == 0);
				success = leafNode->erase(pos, keyComp_, valueComp_, false);
				assert(success);
				if (success) {
					stats_.num_items--;
				}
				leafNeedMerge = leafNode->needMerge();

				saved_success = success;
				result_saved = true;
				// // No merge if leaf is the root.
				// if (stack.size() > 1 && leafNeedMerge) {
				//     stack.back().version = versionNode;
				//     auto stack_copy = stack;
				//     if (!EraseMerge(stack_copy)) {
				//         leafNode->readUnlockOrRestart(versionNode, needRestart);
				//         // The leaf is under-utilize and EraseMerge failed due to conflicts, retry until it succeeds.
				//         // We save the result of delete operation to avoid deleting a key twice.
				//         //goto restart;
				//         return saved_success;
				//     }
				// } else {
				//     leafNode->readUnlockOrRestart(versionNode, needRestart);
				// }
				// return success;
			}

			// Could not find the key.
			// However, we still need to check for under-utilizations.
			result_saved = true;
			saved_success = success;
		}

		if (result_saved == false) {
			result_saved = true;
			saved_success = success;
		}

		// Check if the leaf is under-utilized.
		leafNeedMerge = leafNode->needMerge();
		leafNode->writeUnlock();
		// No merge if leaf is the root.
		if (stack.size() > 1 && leafNeedMerge) {
			versionNode = leafNode->readLockOrRestart(needRestart);
			if (needRestart) {
				assert(result_saved);
				assert(saved_success);
				return saved_success;
			}
			stack.back().version = versionNode;
			auto stack_copy = stack;
			if (!EraseMerge(stack_copy)) {
				// The leaf is under-utilize and EraseMerge failed due to conflict, retry until it succeeds.
				// goto restart;
				return saved_success;
			}
		}

		assert(result_saved);
		assert(saved_success);
		return saved_success;
	}

	bool _remove_and_process_adjacent_tuples(
		const KeyType &deleteKey,
		std::function<bool(const KeyType *prev_key, ValueType *prev_value,
					   const KeyType *cur_key, ValueType *cur_value,
					   const KeyType *next_key, ValueType *next_value)>
			adjacent_tuples_processor)
	{
		// This is the original BTreeOLC adjacent-delete algorithm, with only
		// RegionOffset dereferences and CXL access accounting adapted.
		int restartCount = 0;
		bool saved_success = false;
		bool result_saved = false;
	restart:
		if (restartCount++)
			yield(restartCount, true);
		bool needRestart = false;
		std::vector<StackNodeElement> stack;
		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || node != load_root()) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}
		stack.push_back(StackNodeElement{ node, -1, versionNode });
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;
		while (node->getType() == NodeType::BTreeInner) {
			BTreeInner *inner = static_cast<BTreeInner *>(node);
			if (parent) {
				parent->checkOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}
			parent = inner;
			versionParent = versionNode;
			unsigned child_pos = inner->lowerBound(deleteKey, keyComp_);
			node = inner->childAt(child_pos).get();
			RecordTreeAccess(allocation_, node, false);
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
			stack.push_back(StackNodeElement{ node, int(child_pos), versionNode });
		}

		BTreeLeaf *leafNode = static_cast<BTreeLeaf *>(node);
		BTreeLeaf *leaf = leafNode;
		leafNode->upgradeToWriteLockOrRestart(versionNode, needRestart);
		if (needRestart)
			goto restart;
		if (parent) {
			parent->checkOrRestart(versionParent, needRestart);
			if (needRestart) {
				leafNode->writeUnlock();
				goto restart;
			}
		}

		BTreeLeaf *prevLeaf = nullptr, *nextLeaf = nullptr;
		KeyType *prev_key = nullptr, *cur_key = nullptr, *next_key = nullptr;
		ValueType *prev_value = nullptr, *cur_value = nullptr, *next_value = nullptr;
		unsigned pos = leafNode->lowerBound(deleteKey, keyComp_);
		bool success = false, leafNeedMerge = false;
		if (pos < leafNode->getCount() && !saved_success) {
			const KeyType &key = leafNode->keys_[pos];
			if (keyComp_(key, deleteKey) == 0) {
				cur_key = &leaf->keys_[pos];
				cur_value = &leaf->values_[pos];
				if (pos > 0) {
					prev_key = &leaf->keys_[pos - 1];
					prev_value = &leaf->values_[pos - 1];
				} else {
					prevLeaf = leaf->pre_.get();
					if (prevLeaf) {
						RecordTreeAccess(allocation_, prevLeaf, true);
						prevLeaf->writeLockOrRestart(needRestart);
						if (needRestart) {
							leaf->writeUnlock();
							goto restart;
						}
						if (prevLeaf->getCount() > 0) {
							prev_key = &prevLeaf->keys_[prevLeaf->getCount() - 1];
							prev_value = &prevLeaf->values_[prevLeaf->getCount() - 1];
						}
					}
				}
				if (pos < leaf->getCount() - 1) {
					next_key = &leaf->keys_[pos + 1];
					next_value = &leaf->values_[pos + 1];
				} else {
					nextLeaf = leaf->next_.get();
					if (nextLeaf) {
						RecordTreeAccess(allocation_, nextLeaf, true);
						nextLeaf->writeLockOrRestart(needRestart);
						if (needRestart) {
							leaf->writeUnlock();
							if (prevLeaf)
								prevLeaf->writeUnlock();
							goto restart;
						}
						if (nextLeaf->getCount() > 0) {
							next_key = &nextLeaf->keys_[0];
							next_value = &nextLeaf->values_[0];
						}
					}
				}
				bool should_remove = adjacent_tuples_processor(
					prev_key, prev_value, cur_key, cur_value, next_key, next_value);
				if (should_remove) {
					assert(keyComp_(leafNode->keys_[pos], deleteKey) == 0);
					success = leafNode->erase(pos, keyComp_, valueComp_, false);
					assert(success);
					if (success)
						stats_.num_items--;
					leafNeedMerge = leafNode->needMerge();
					saved_success = success;
					result_saved = true;
				}
				if (prevLeaf)
					prevLeaf->writeUnlock();
				if (nextLeaf)
					nextLeaf->writeUnlock();
			}
			result_saved = true;
			saved_success = success;
		}
		if (!result_saved) {
			result_saved = true;
			saved_success = success;
		}
		leafNeedMerge = leafNode->needMerge();
		leafNode->writeUnlock();
		if (stack.size() > 1 && leafNeedMerge) {
			versionNode = leafNode->readLockOrRestart(needRestart);
			if (needRestart) {
				assert(result_saved);
				assert(saved_success);
				return saved_success;
			}
			stack.back().version = versionNode;
			auto stack_copy = stack;
			if (!EraseMerge(stack_copy))
				return saved_success;
		}
		assert(result_saved);
		assert(saved_success);
		return saved_success;
	}

	bool _lookupForUpdate(const KeyType &key, std::function<void(const KeyType &key, ValueType &value)> update_processor)
	{
		int restartCount = 0;
restart:
		if (restartCount++)
			yield(restartCount);
		bool needRestart = false;

		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || (node != load_root())) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;

		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			versionParent = versionNode;

			node = inner->childAt(inner->lowerBound(key, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			prefetch((char *)node, sizeof(NodeMetaData));
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		}
		node->checkOrRestart(versionNode, needRestart);
		if (needRestart)
			goto restart;

		auto leaf = static_cast<BTreeLeaf *>(node);
		leaf->upgradeToWriteLockOrRestart(versionNode, needRestart);
		if (needRestart) {
			goto restart;
		}
		if (parent) {
			// check the version only, don't call `readUnlockOrRestart`
			parent->checkOrRestart(versionParent, needRestart);
			if (needRestart) {
				leaf->writeUnlock();
				goto restart;
			}
		}

		unsigned pos = leaf->lowerBound(key, keyComp_);

		bool success = false;
		if ((pos < leaf->getCount()) && keyComp_(leaf->keys_[pos], key) == 0) {
			success = true;
			RecordTreeDataRead(&leaf->keys_[pos], sizeof(KeyType));
			RecordTreeDataWrite(&leaf->values_[pos], sizeof(ValueType));
			update_processor(leaf->keys_[pos], leaf->values_[pos]);
		}

		leaf->writeUnlock();
		return success;
	}

	bool _lookupForNextKeyUpdate(
		const KeyType &key,
		std::function<void(const KeyType *, ValueType *, const KeyType *,
					   ValueType *, const KeyType *, ValueType *)> processor)
	{
		int restartCount = 0;
	restart:
		if (restartCount++) yield(restartCount);
		bool needRestart = false;
		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || node != load_root()) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;
		while (node->getType() == NodeType::BTreeInner) {
			auto *inner = static_cast<BTreeInner *>(node);
			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart) goto restart;
			}
			parent = inner;
			versionParent = versionNode;
			node = inner->childAt(inner->lowerBound(key, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			prefetch((char *)node, sizeof(NodeMetaData));
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart) goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart) goto restart;
		}
		node->checkOrRestart(versionNode, needRestart);
		if (needRestart) goto restart;
		auto *leaf = static_cast<BTreeLeaf *>(node);
		leaf->upgradeToWriteLockOrRestart(versionNode, needRestart);
		if (needRestart) goto restart;
		if (parent) {
			parent->checkOrRestart(versionParent, needRestart);
			if (needRestart) { leaf->writeUnlock(); goto restart; }
		}

		bool success = false;
		BTreeLeaf *prevLeaf = nullptr, *nextLeaf = nullptr;
		KeyType *prev_key = nullptr, *cur_key = nullptr, *next_key = nullptr;
		ValueType *prev_value = nullptr, *cur_value = nullptr, *next_value = nullptr;
		unsigned pos = leaf->lowerBound(key, keyComp_);
		if (pos < leaf->getCount() && keyComp_(leaf->keys_[pos], key) == 0) {
			success = true;
			cur_key = &leaf->keys_[pos];
			cur_value = &leaf->values_[pos];
			if (pos > 0) {
				prev_key = &leaf->keys_[pos - 1];
				prev_value = &leaf->values_[pos - 1];
			} else if ((prevLeaf = leaf->pre_.get()) != nullptr) {
				RecordTreeAccess(allocation_, prevLeaf, true);
				prevLeaf->writeLockOrRestart(needRestart);
				if (needRestart) { leaf->writeUnlock(); goto restart; }
				if (prevLeaf->getCount() > 0) {
					prev_key = &prevLeaf->keys_[prevLeaf->getCount() - 1];
					prev_value = &prevLeaf->values_[prevLeaf->getCount() - 1];
				}
			}
			if (pos < leaf->getCount() - 1) {
				next_key = &leaf->keys_[pos + 1];
				next_value = &leaf->values_[pos + 1];
			} else if ((nextLeaf = leaf->next_.get()) != nullptr) {
				RecordTreeAccess(allocation_, nextLeaf, true);
				nextLeaf->writeLockOrRestart(needRestart);
				if (needRestart) {
					leaf->writeUnlock();
					if (prevLeaf) prevLeaf->writeUnlock();
					goto restart;
				}
				if (nextLeaf->getCount() > 0) {
					next_key = &nextLeaf->keys_[0];
					next_value = &nextLeaf->values_[0];
				}
			}
			processor(prev_key, prev_value, cur_key, cur_value, next_key, next_value);
		}
		leaf->writeUnlock();
		if (prevLeaf) prevLeaf->writeUnlock();
		if (nextLeaf) nextLeaf->writeUnlock();
		return success;
	}

	/**
	 * @param flag find key and it corresponding value when flag is true
	 */
	bool _lookup(const KeyValuePair &element, ValueType &result, bool flag)
	{
		int restartCount = 0;
restart:
		if (restartCount++)
			yield(restartCount);
		bool needRestart = false;

		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || (node != load_root())) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;

		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			versionParent = versionNode;

			node = inner->childAt(inner->lowerBound(element.first, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			prefetch((char *)node, sizeof(NodeMetaData));
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		}
		auto leaf = static_cast<BTreeLeaf *>(node);
		unsigned pos = leaf->lowerBound(element.first, keyComp_);
		bool success = false;
		if (pos < leaf->getCount())
			RecordTreeDataRead(&leaf->keys_[pos], sizeof(KeyType));
		if ((pos < leaf->getCount()) && keyComp_(leaf->keys_[pos], element.first) == 0) {
			RecordTreeDataRead(&leaf->values_[pos], sizeof(ValueType));
			if (flag) {
				success = true;
				result = leaf->values_[pos];
			} else {
				if (valueComp_(element.second, leaf->values_[pos]) == 0) {
					success = true;
					result = leaf->values_[pos];
				}
			}
		}
		if (parent) {
			parent->readUnlockOrRestart(versionParent, needRestart);
			if (needRestart) {
				goto restart;
			}
		}
		node->readUnlockOrRestart(versionNode, needRestart);
		if (needRestart)
			goto restart;

		return success;
	}

	/**
	 * find key and it's corresponding value
	 */
	bool _lookup(const KeyType &key, ValueType &result)
	{
		int restartCount = 0;
restart:
		if (restartCount++)
			yield(restartCount);
		bool needRestart = false;

		NodeBase *node = load_root();
		RecordTreeAccess(allocation_, node, false);
		uint64_t versionNode = node->readLockOrRestart(needRestart);
		if (needRestart || (node != load_root())) {
			node->readUnlockOrRestart(versionNode, needRestart);
			goto restart;
		}

		// Parent of current node
		BTreeInner *parent = nullptr;
		uint64_t versionParent = 0;

		while (node->getType() == NodeType::BTreeInner) {
			auto inner = static_cast<BTreeInner *>(node);

			if (parent) {
				parent->readUnlockOrRestart(versionParent, needRestart);
				if (needRestart)
					goto restart;
			}

			parent = inner;
			versionParent = versionNode;

			node = inner->childAt(inner->lowerBound(key, keyComp_)).get();
			RecordTreeAccess(allocation_, node, false);
			prefetch((char *)node, sizeof(NodeMetaData));
			inner->checkOrRestart(versionNode, needRestart);
			if (needRestart)
				goto restart;
			versionNode = node->readLockOrRestart(needRestart);
			if (needRestart)
				goto restart;
		}
		node->checkOrRestart(versionNode, needRestart);
		if (needRestart)
			goto restart;
		auto leaf = static_cast<BTreeLeaf *>(node);
		unsigned pos = leaf->lowerBound(key, keyComp_);
		bool success = false;
		if ((pos < leaf->getCount()) && keyComp_(leaf->keys_[pos], key) == 0) {
			RecordTreeDataRead(&leaf->keys_[pos], sizeof(KeyType));
			RecordTreeDataRead(&leaf->values_[pos], sizeof(ValueType));
			success = true;
			result = leaf->values_[pos];
		}
		if (parent) {
			parent->readUnlockOrRestart(versionParent, needRestart);
			if (needRestart) {
				leaf->readUnlockOrRestart(versionNode, needRestart);
				goto restart;
			}
		}
		node->readUnlockOrRestart(versionNode, needRestart);
		if (needRestart)
			goto restart;
		return success;
	}
};

}
