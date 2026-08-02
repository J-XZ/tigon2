//
// Created by Yi Lu on 7/18/18.
//

#pragma once

#include "common/CCHashTable.h"
#include "common/btree_olc_cxl/BTreeOLC_CXL.h"


namespace star
{

struct LegacyOffsetPtrRowReference {
        using StoredRow = btreeolc_cxl::PersistentOffset<void>;
        StoredRow Null() const { return StoredRow(nullptr); }
        bool IsNull(const StoredRow &row) const { return row.get() == nullptr; }
        StoredRow Encode(void *row) const { return StoredRow(row); }
        void *Resolve(const StoredRow &row) const { return row.get(); }
        bool Equal(const StoredRow &left, const StoredRow &right) const {
                return left.get() == right.get();
        }
};

class CXLTableBase {
    public:
	virtual ~CXLTableBase() = default;

	virtual void *search(const void *key) = 0;

        virtual void scan(const void *min_key, std::function<bool(const void *, void *, bool)> scan_processor) = 0;

	virtual bool insert(const void *key, void *row, bool is_placeholder = false) = 0;

        virtual bool remove(const void *key, void *row) = 0;

	virtual std::size_t tableID() = 0;

	virtual std::size_t partitionID() = 0;
};

template <class KeyType> class CXLTableHashMap : public CXLTableBase {
    public:
	virtual ~CXLTableHashMap() override = default;

        CXLTableHashMap(CCHashTable *cxl_hashtable, std::size_t tableID, std::size_t partitionID)
		: cxl_hashtable_(cxl_hashtable)
                , tableID_(tableID)
		, partitionID_(partitionID)
	{
	}

	virtual void *search(const void *key) override
        {
                const auto &k = *static_cast<const KeyType *>(key);
                return cxl_hashtable_->search(k.get_plain_key());
        }

        virtual void scan(const void *min_key, std::function<bool(const void *, void *, bool)> scan_processor) override
        {
                CHECK(0);
        }

	virtual bool insert(const void *key, void *row, bool is_placeholder = false) override
        {
                CHECK(is_placeholder == false);
                const auto &k = *static_cast<const KeyType *>(key);
                return cxl_hashtable_->insert(k.get_plain_key(), reinterpret_cast<char *>(row));
        }

        virtual bool remove(const void *key, void *row) override
        {
                const auto &k = *static_cast<const KeyType *>(key);
                return cxl_hashtable_->remove(k.get_plain_key(), reinterpret_cast<char *>(row));
        }

	virtual std::size_t tableID() override
        {
                return tableID_;
        }

	virtual std::size_t partitionID() override
        {
                return partitionID_;
        }

    private:
	CCHashTable *cxl_hashtable_;
	std::size_t tableID_;
	std::size_t partitionID_;
};

template <class KeyType, class KeyComparator,
          class SharedRowReferencePolicy = LegacyOffsetPtrRowReference>
class CXLTableBTreeOLC : public CXLTableBase {
    public:
        static constexpr uint64_t update_threshold = 1024;
        static constexpr uint64_t leaf_page_size = 4096;
        static constexpr uint64_t inner_page_size = 4096;

        // std::atomic has implicitly deleted copy-constructor
        // so we need to define a ValueType that supports it
        struct BTreeOLCValue {
                BTreeOLCValue() = default;

                BTreeOLCValue(const BTreeOLCValue &value)
                {
                        this->row = value.row;
                        const bool valid = latency_sim::CountedAtomicLoad(
                            value.is_valid, std::memory_order_relaxed,
                            latency_sim::AtomicDomain::kLocalDram);
                        latency_sim::CountedAtomicStore(
                            this->is_valid, valid, std::memory_order_relaxed,
                            latency_sim::AtomicDomain::kLocalDram);
                }

                BTreeOLCValue &operator=(const BTreeOLCValue &value)
                {
                        this->row = value.row;
                        const bool valid = latency_sim::CountedAtomicLoad(
                            value.is_valid, std::memory_order_relaxed,
                            latency_sim::AtomicDomain::kLocalDram);
                        latency_sim::CountedAtomicStore(
                            this->is_valid, valid, std::memory_order_relaxed,
                            latency_sim::AtomicDomain::kLocalDram);
                        return *this;
                }

                typename SharedRowReferencePolicy::StoredRow row{};
                std::atomic<bool> is_valid{ false };
        };

        struct BTreeOLCValueComparator {
                int operator()(const BTreeOLCValue &a, const BTreeOLCValue &b) const
                {
                        if (a.row == b.row)
                                return 0;
                        else
                                return 1;
                }
        };

        using CXLBTree = btreeolc_cxl::BPlusTree<KeyType, BTreeOLCValue, KeyComparator, BTreeOLCValueComparator, update_threshold, leaf_page_size, inner_page_size>;
        using StoredRow = typename SharedRowReferencePolicy::StoredRow;

	virtual ~CXLTableBTreeOLC() override = default;

        CXLTableBTreeOLC(CXLBTree *cxl_btree, std::size_t tableID, std::size_t partitionID,
                         SharedRowReferencePolicy row_policy = SharedRowReferencePolicy{})
                : cxl_btree_(cxl_btree)
                , tableID_(tableID)
		, partitionID_(partitionID)
		, row_policy_(std::move(row_policy))
	{
	}

	virtual void *search(const void *key) override
        {
                StoredRow row = row_policy_.Null();
                return lookup_reference(key, &row) ? row_policy_.Resolve(row)
                                                   : nullptr;
        }

        bool lookup_reference(const void *key, StoredRow *row)
        {
                if (row == nullptr) throw std::invalid_argument("null shared row output");
                const auto &k = *static_cast<const KeyType *>(key);
                BTreeOLCValue value;
                if (!cxl_btree_->lookup(k, value)) return false;
                if (!latency_sim::CountedAtomicLoad(
                        value.is_valid, std::memory_order_relaxed,
                        latency_sim::AtomicDomain::kLocalDram)) return false;
                *row = value.row;
                return !row_policy_.IsNull(*row);
        }

        virtual void scan(const void *min_key, std::function<bool(const void *, void *, bool)> scan_processor) override
        {
                const auto &min_k = *static_cast<const KeyType *>(min_key);

                auto processor = [&](const KeyType &key, BTreeOLCValue &value, bool is_last_tuple) -> bool {
                        bool should_end = scan_processor(&key, row_policy_.Resolve(value.row), is_last_tuple);

                        if (should_end == false) {
                                return false;
                        } else {
                                return true;
                        }
		};

                cxl_btree_->scanForUpdate(min_k, processor);
        }

	virtual bool insert(const void *key, void *row, bool is_placeholder = false) override
        {
                const auto &k = *static_cast<const KeyType *>(key);

                BTreeOLCValue value;
                value.row = row_policy_.Encode(row);
                latency_sim::CountedAtomicStore(
                    value.is_valid, is_placeholder == false,
                    std::memory_order_relaxed,
                    latency_sim::AtomicDomain::kLocalDram);

		bool success = cxl_btree_->insert(k, value);
		return success;
        }

        virtual bool remove(const void *key, void *row) override
        {
                const auto &k = *static_cast<const KeyType *>(key);

                bool success = cxl_btree_->remove(k);
                CHECK(success == true);

                return success;
        }

	virtual std::size_t tableID() override
        {
                return tableID_;
        }

	virtual std::size_t partitionID() override
        {
                return partitionID_;
        }

    private:
	CXLBTree *cxl_btree_;
	std::size_t tableID_;
	std::size_t partitionID_;
        SharedRowReferencePolicy row_policy_;
};

} // namespace star
