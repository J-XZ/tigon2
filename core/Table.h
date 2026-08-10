//
// Created by Yi Lu on 7/18/18.
//

#pragma once

#include <thread>
#include <memory>
#include "benchmark/tpcc/Schema.h"
#include "common/ClassOf.h"
#include "common/Encoder.h"
#include "common/HashMap.h"
#include "common/StringPiece.h"
#include "common/btree_olc/BTreeOLC.h"

static thread_local uint64_t tid = std::numeric_limits<uint64_t>::max();
extern bool do_tid_check;
static std::hash<std::thread::id> tid_hasher;
namespace star
{

extern void tid_check();

class ITable {
    public:
        enum { HASHMAP, BTREE };

	using MetaDataType = std::atomic<uint64_t>;

        class row_entity
        {
            public:
                row_entity() = default;
                row_entity(const void *key, uint64_t key_size, MetaDataType *meta, void *data, uint64_t row_size)
                        : key_size(key_size)
                        , meta(meta)
                        , data(data)
                        , row_size(row_size)
                {
                        memcpy(this->key, key, key_size);
                }

                static constexpr uint64_t max_key_size = 64;
                char key[max_key_size];
                uint64_t key_size;
                MetaDataType *meta{ nullptr };
                void *data{ nullptr };
                uint64_t row_size;
        };

	virtual ~ITable() = default;

        virtual uint64_t get_plain_key(const void *key) = 0;

        virtual int compare_key(const void *a, const void *b) = 0;

	virtual std::tuple<MetaDataType *, void *> search(const void *key) = 0;

	virtual bool contains(const void *key)
	{
		return true;
	}

	virtual void *search_value(const void *key) = 0;

	virtual MetaDataType *search_metadata(const void *key) = 0;

        virtual void scan(const void *min_key, std::function<bool(const void *, MetaDataType *, void *, bool)> scan_processor) = 0;

	virtual bool insert(const void *key, const void *value, bool is_placeholder = false) = 0;

        virtual bool insert_lock_next_key(const void *key, const void *value, std::function<bool(const void *, MetaDataType *, void *)> next_key_processor, bool is_placeholder = false) = 0;

        virtual bool insert_and_process_adjacent_tuples(const void *key, const void *value,
                std::function<bool(const void *prev_key, MetaDataType *prev_meta, void *prev_data, const void *next_key, MetaDataType *next_meta, void *next_data)> update_processor,
                bool is_placeholder = false) = 0;

        virtual bool remove(const void *key) = 0;

        virtual bool remove_and_process_adjacent_tuples(const void *key,
                std::function<bool(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> processor) = 0;

	virtual void update(
		const void *key, const void *value, std::function<void(const void *, const void *)> on_update = [](const void *, const void *) {}) = 0;

        virtual bool search_and_update_next_key_info(const void *key,
                std::function<void(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> update_processor) = 0;

	virtual void deserialize_value(const void *key, StringPiece stringPiece) = 0;

	virtual void serialize_value(Encoder &enc, const void *value) = 0;

	virtual std::size_t key_size() = 0;

	virtual std::size_t value_size() = 0;

	virtual std::size_t field_size() = 0;

	virtual std::size_t tableID() = 0;

	virtual std::size_t partitionID() = 0;

        virtual int tableType() = 0;

	virtual void turn_on_cow()
	{
	}

	virtual void dump_copy(std::function<void(const void *, const void *)> dump_processor, std::function<void()> unlock_processor)
	{
	}

	virtual bool cow_dump_finished()
	{
		return true;
	}

	virtual std::function<void()> turn_off_cow()
	{
		return []() {};
	}

        virtual void move_all_into_cxl(std::function<bool(ITable *, const void *, std::tuple<MetaDataType *, void *> &, bool)> move_in_func)
        {
                CHECK(0);
        }
};

class MetaInitFuncNothing {
    public:
	uint64_t operator()(bool is_tuple_valid = true)
	{
		return 0;
	}
};

extern uint64_t SundialMetadataInit(bool is_tuple_valid);
class MetaInitFuncSundial {
    public:
	uint64_t operator()(bool is_tuple_valid = true)
	{
		return SundialMetadataInit(is_tuple_valid);
	}
};

extern uint64_t SundialPashaMetadataLocalInit(bool is_tuple_valid);
class MetaInitFuncSundialPasha {
    public:
	uint64_t operator()(bool is_tuple_valid = true)
	{
		return SundialPashaMetadataLocalInit(is_tuple_valid);
	}
};

extern uint64_t TwoPLMetadataInit(bool is_tuple_valid);
class MetaInitFuncTwoPL {
    public:
	uint64_t operator()(bool is_tuple_valid = true)
	{
		return TwoPLMetadataInit(is_tuple_valid);
	}
};

extern uint64_t TwoPLPashaMetadataLocalInit(bool is_tuple_valid);
class MetaInitFuncTwoPLPasha {
    public:
	uint64_t operator()(bool is_tuple_valid = true)
	{
		return TwoPLPashaMetadataLocalInit(is_tuple_valid);
	}
};

template <std::size_t N, class KeyType, class ValueType, class KeyComparator, class ValueComparator, class MetaInitFunc = MetaInitFuncNothing> class TableHashMap : public ITable {
    public:
	using MetaDataType = std::atomic<uint64_t>;

	virtual ~TableHashMap() override = default;

	TableHashMap(std::size_t tableID, std::size_t partitionID)
		: tableID_(tableID)
		, partitionID_(partitionID)
	{
	}

        uint64_t get_plain_key(const void *key) override
        {
                tid_check();
                const auto &k = *static_cast<const KeyType *>(key);
                return k.get_plain_key();
        }

        int compare_key(const void *a, const void *b) override
        {
                const auto &k_a = *static_cast<const KeyType *>(a);
                const auto &k_b = *static_cast<const KeyType *>(b);
                return KeyComparator()(k_a, k_b);
        }

	std::tuple<MetaDataType *, void *> search(const void *key) override
	{
                CHECK(contains(key) == true);
		tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
                CHECK(map_.contains(k) == true);
		auto &v = map_[k];
		return std::make_tuple(&std::get<0>(v), &std::get<1>(v));
	}

	void *search_value(const void *key) override
	{
                CHECK(contains(key) == true);
		tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
		return &std::get<1>(map_[k]);
	}

	MetaDataType *search_metadata(const void *key) override
	{
                CHECK(contains(key) == true);
		tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
		return &std::get<0>(map_[k]);
	}

	bool contains(const void *key) override
	{
		const auto &k = *static_cast<const KeyType *>(key);
		return map_.contains(k);
	}

        void scan(const void *min_key, std::function<bool(const void *key, MetaDataType *meta, void *data, bool)> scan_processor) override
        {
                // hash table does not support scan
                CHECK(0);
        }

        bool insert(const void *key, const void *value, bool is_placeholder = false) override
	{
                // hash table does not handle concurrent inserts, so it will always succeed
		tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
		const auto &v = *static_cast<const ValueType *>(value);
		bool ok = map_.contains(k);
		DCHECK(ok == false);
		auto &row = map_[k];
		std::get<0>(row).store(MetaInitFunc()());
		std::get<1>(row) = v;

                return true;
	}

        bool insert_lock_next_key(const void *key, const void *value, std::function<bool(const void *, MetaDataType *, void *)> next_key_processor, bool is_placeholder = false) override
        {
                CHECK(0);
        }

        bool insert_and_process_adjacent_tuples(const void *key, const void *value,
                std::function<bool(const void *prev_key, MetaDataType *prev_meta, void *prev_data, const void *next_key, MetaDataType *next_meta, void *next_data)> update_processor,
                bool is_placeholder = false) override
        {
                CHECK(0);
        }

        bool remove(const void *key) override
        {
                CHECK(0);
        }

        bool remove_and_process_adjacent_tuples(const void *key,
                std::function<bool(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> processor) override
        {
                CHECK(0);
        }

	void update(const void *key, const void *value, std::function<void(const void *, const void *)> on_update) override
	{
		tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
		const auto &v = *static_cast<const ValueType *>(value);
		auto &row = map_[k];
		on_update(key, &std::get<1>(row));
		std::get<1>(row) = v;
	}

        bool search_and_update_next_key_info(const void *key,
                std::function<void(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> update_processor) override
        {
                // no need to maintain next-key information for unordered tables
                return true;
        }

	void deserialize_value(const void *key, StringPiece stringPiece) override
	{
		tid_check();
		std::size_t size = stringPiece.size();
		const auto &k = *static_cast<const KeyType *>(key);
		auto &row = map_[k];
		auto &v = std::get<1>(row);

		Decoder dec(stringPiece);
		dec >> v;

		DCHECK(size - dec.size() == ClassOf<ValueType>::size());
	}

	void serialize_value(Encoder &enc, const void *value) override
	{
		tid_check();
		std::size_t size = enc.size();
		const auto &v = *static_cast<const ValueType *>(value);
		enc << v;

		DCHECK(enc.size() - size == ClassOf<ValueType>::size());
	}

	std::size_t key_size() override
	{
		tid_check();
		return sizeof(KeyType);
	}

	std::size_t value_size() override
	{
		tid_check();
		return sizeof(ValueType);
	}

	std::size_t field_size() override
	{
		tid_check();
		return ClassOf<ValueType>::size();
	}

	std::size_t tableID() override
	{
		tid_check();
		return tableID_;
	}

	std::size_t partitionID() override
	{
		tid_check();
		return partitionID_;
	}

        int tableType() override
        {
                return HASHMAP;
        }

        void move_all_into_cxl(std::function<bool(ITable *, const void *, std::tuple<MetaDataType *, void *> &, bool)> move_in_func) override
        {
                auto processor = [&](const KeyType &key, std::tuple<MetaDataType, ValueType> &row) {
                        MetaDataType *meta_ptr = &std::get<0>(row);
                        ValueType *data_ptr = &std::get<1>(row);
                        std::tuple<MetaDataType *, void *> row_tuple(meta_ptr, data_ptr);
			bool ret = move_in_func(this, &key, row_tuple, false);
		};

                map_.iterate_non_const(processor, []() {});
        }

    private:
	HashMap<N, KeyType, std::tuple<MetaDataType, ValueType> > map_;
	std::size_t tableID_;
	std::size_t partitionID_;
};

template <class ValueType, class MetaInitFunc>
struct RawPointerRowStorage {
        using MetaDataType = std::atomic<uint64_t>;
        struct ValueStruct {
                MetaDataType meta;
                ValueType data;
        };
        using StoredRow = ValueStruct *;
        static void Store(StoredRow *destination, StoredRow value) {
                *destination = value;
        }
        static StoredRow Load(const StoredRow *source) {
                return *source;
        }
        static void Copy(StoredRow *destination, const StoredRow *source) {
                *destination = *source;
        }
        StoredRow Null() const { return nullptr; }
        bool IsNull(StoredRow row) const { return row == nullptr; }
        MetaDataType *Meta(StoredRow row) const { return row == nullptr ? nullptr : &row->meta; }
        void *Data(StoredRow row) const { return row == nullptr ? nullptr : &row->data; }
        void *AdjacentMeta(StoredRow row) const {
                return row == nullptr ? nullptr
                                       : reinterpret_cast<void *>(Meta(row)->load());
        }
        StoredRow AllocateAndConstruct(const void *value, bool is_placeholder) const {
                auto *row = new ValueStruct;
                row->meta.store(MetaInitFunc()(!is_placeholder), std::memory_order_relaxed);
                row->data = *static_cast<const ValueType *>(value);
                return row;
        }
        void DestroyUnpublished(StoredRow row) const { delete row; }
        void Retire(StoredRow) const {}
        void Update(StoredRow row, const void *value) const {
                row->data = *static_cast<const ValueType *>(value);
        }
        std::size_t ValueSize() const { return sizeof(ValueType); }
        std::size_t KeySize(std::size_t key_size) const { return key_size; }
        std::size_t FieldSize() const { return ClassOf<ValueType>::size(); }
        void Deserialize(StoredRow row, StringPiece bytes) const {
                Decoder decoder(bytes);
                decoder >> row->data;
                DCHECK(bytes.size() - decoder.size() == FieldSize());
        }
        void Serialize(Encoder &encoder, const void *value) const {
                encoder << *static_cast<const ValueType *>(value);
        }
        template <typename BTree>
        std::unique_ptr<BTree> MakeTree() const { return std::make_unique<BTree>(); }
        template <typename BTree>
        void BindPublishedRoot(BTree &, std::atomic<uint64_t> *) const {}
        template <typename BTree>
        uint64_t RootOffset(const BTree &) const { return 0; }
};

template <class KeyType, class ValueType, class KeyComparator,
          class ValueComparator, class MetaInitFunc = MetaInitFuncNothing,
          template <class, class, class, class, std::size_t, uint64_t, uint64_t>
          class BTreeTemplate = btreeolc::BPlusTree,
          class RowStoragePolicy = RawPointerRowStorage<ValueType, MetaInitFunc>>
class TableBTreeOLC : public ITable {
    public:
        using MetaDataType = std::atomic<uint64_t>;

        static constexpr uint64_t update_threshold = 1024;
        static constexpr uint64_t leaf_page_size = 4096;
        static constexpr uint64_t inner_page_size = 4096;

        using ValueStruct = typename RowStoragePolicy::ValueStruct;
        using StoredRow = typename RowStoragePolicy::StoredRow;

        // std::atomic has implicitly deleted copy-constructor
        // so we need to define a ValueType that supports it
        struct BTreeOLCValue {
                BTreeOLCValue() {
                        RowStoragePolicy::Store(&row, StoredRow{});
                }

                BTreeOLCValue(const BTreeOLCValue &value)
                {
                        RowStoragePolicy::Copy(&this->row, &value.row);
                }

                BTreeOLCValue &operator=(const BTreeOLCValue &value)
                {
                        RowStoragePolicy::Copy(&this->row, &value.row);
                        return *this;
                }

                StoredRow row;
        };

        struct BTreeOLCValueComparator {
                int operator()(const BTreeOLCValue &a, const BTreeOLCValue &b) const
                {
                        if (RowStoragePolicy::Load(&a.row) ==
                            RowStoragePolicy::Load(&b.row))
                                return 0;
                        else
                                return 1;
                }
        };

        using BTree = BTreeTemplate<KeyType, BTreeOLCValue, KeyComparator,
                                    BTreeOLCValueComparator, update_threshold,
                                    leaf_page_size, inner_page_size>;

	virtual ~TableBTreeOLC() override = default;

	TableBTreeOLC(std::size_t tableID, std::size_t partitionID,
                      RowStoragePolicy storage = RowStoragePolicy{})
		: tableID_(tableID)
		, partitionID_(partitionID)
		, storage_(std::move(storage))
	{
	        btree_ = storage_.template MakeTree<BTree>();
	}

        uint64_t get_plain_key(const void *key) override
        {
                tid_check();
                const auto &k = *static_cast<const KeyType *>(key);
                return k.get_plain_key();
        }

        int compare_key(const void *a, const void *b) override
        {
                const auto &k_a = *static_cast<const KeyType *>(a);
                const auto &k_b = *static_cast<const KeyType *>(b);
                return KeyComparator()(k_a, k_b);
        }

	std::tuple<MetaDataType *, void *> search(const void *key) override
	{
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);

                BTreeOLCValue value;
                bool success = btree_->lookup(k, value);

                if (success == true) {
                        return std::make_tuple(
                            storage_.Meta(storage_.Load(&value.row)),
                            storage_.Data(storage_.Load(&value.row)));
                } else {
                        return std::make_tuple(nullptr, nullptr);
                }
	}

	void *search_value(const void *key) override
	{
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);

                BTreeOLCValue value;
                bool success = btree_->lookup(k, value);

                if (success == true) {
                        return storage_.Data(storage_.Load(&value.row));
                } else {
                        return nullptr;
                }
	}

	MetaDataType *search_metadata(const void *key) override
	{
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);

                BTreeOLCValue value;
                bool success = btree_->lookup(k, value);

                if (success == true) {
                        return storage_.Meta(storage_.Load(&value.row));
                } else {
                        return nullptr;
                }
	}

	bool contains(const void *key) override
	{
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);

                BTreeOLCValue value;
                bool success = btree_->lookup(k, value);

                if (success) {
                        return true;
                } else {
                        return false;
                }
	}

        void scan(const void *min_key, std::function<bool(const void *, MetaDataType *, void *, bool)> scan_processor) override
        {
                tid_check();
                const auto &min_k = *static_cast<const KeyType *>(min_key);

                auto processor = [&](const KeyType &key, BTreeOLCValue &value, bool is_last_tuple) -> bool {
                        const StoredRow row = storage_.Load(&value.row);
                        MetaDataType *meta_ptr = storage_.Meta(row);
                        void *data_ptr = storage_.Data(row);

                        bool should_end = scan_processor(&key, meta_ptr, data_ptr, is_last_tuple);

                        if (should_end == false) {
                                CHECK(KeyComparator()(key, min_k) >= 0);
                                return false;
                        } else {
                                return true;
                        }
		};

                btree_->scanForUpdate(min_k, processor);
        }

        // used by other baselines
	bool insert(const void *key, const void *value, bool is_placeholder = false) override
	{
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
                bool is_tuple_valid = !is_placeholder;

                // create value that will not be moved around
		StoredRow row = storage_.AllocateAndConstruct(value, is_placeholder);

                // BTreeOLCValue will be moved around and thus only stores pointers to the actual value
                BTreeOLCValue btree_value;
                btree_value.row = row;

                // insert BTreeOLCValue to BTreeOLC
		bool success = btree_->insert(k, btree_value);
		if (!success) storage_.DestroyUnpublished(row);
		return success;
	}

        // used by TwoPL
        bool insert_lock_next_key(const void *key, const void *value, std::function<bool(const void *, MetaDataType *, void *)> next_key_processor, bool is_placeholder = false) override
	{
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
                CHECK(is_placeholder == true);

                auto processor = [&](const KeyType *key, BTreeOLCValue *value) -> bool {
                        const StoredRow row = storage_.Load(&value->row);
                        MetaDataType *meta_ptr = storage_.Meta(row);
                        void *data_ptr = storage_.Data(row);

                        return next_key_processor(key, meta_ptr, data_ptr);
		};

                bool is_tuple_valid = !is_placeholder;

                // create value that will not be moved around
		StoredRow row = storage_.AllocateAndConstruct(value, is_placeholder);

                // BTreeOLCValue will be moved around and thus only stores pointers to the actual value
                BTreeOLCValue btree_value;
                btree_value.row = row;

                // insert BTreeOLCValue to BTreeOLC
		bool success = btree_->insert_lock_next_key(k, btree_value, processor);
		if (!success) storage_.DestroyUnpublished(row);
		return success;
	}

        // used by Pasha
        bool insert_and_process_adjacent_tuples(const void *key, const void *value,
                std::function<bool(const void *prev_key, MetaDataType *prev_meta, void *prev_data, const void *next_key, MetaDataType *next_meta, void *next_data)> processor,
                bool is_placeholder = false) override
	{
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
                CHECK(is_placeholder == true);

                bool is_tuple_valid = !is_placeholder;

                // create value that will not be moved around
		StoredRow row = storage_.AllocateAndConstruct(value, is_placeholder);

                // BTreeOLCValue will be moved around and thus only stores pointers to the actual value
                BTreeOLCValue btree_value;
                btree_value.row = row;

                auto adjacent_tuples_processor = [&](const KeyType *prev_key, BTreeOLCValue *prev_value, const KeyType *next_key, BTreeOLCValue *next_value) -> bool {
                        MetaDataType *prev_meta_ptr = nullptr, *next_meta_ptr = nullptr;
                        void *prev_data = nullptr, *next_data = nullptr;

                        if (prev_value != nullptr) {
                                const StoredRow row = storage_.Load(&prev_value->row);
                                prev_meta_ptr = storage_.Meta(row);
                                prev_data = storage_.Data(row);
                        }
                        if (next_value != nullptr) {
                                const StoredRow row = storage_.Load(&next_value->row);
                                next_meta_ptr = storage_.Meta(row);
                                next_data = storage_.Data(row);
                        }

                        return processor(prev_key, prev_meta_ptr, prev_data, next_key, next_meta_ptr, next_data);
		};

                // insert BTreeOLCValue to BTreeOLC
		bool success = btree_->insert_and_process_adjacent_tuples(k, btree_value, adjacent_tuples_processor);
		if (!success) storage_.DestroyUnpublished(row);
		return success;
	}

        bool remove(const void *key) override
        {
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);

                bool success = btree_->remove(k);
                CHECK(success == true);

                return success;
        }

        bool remove_and_process_adjacent_tuples(const void *key,
                std::function<bool(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> processor) override
        {
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
                StoredRow removed_row = storage_.Null();

                auto adjacent_tuples_processor = [&](const KeyType *prev_key, BTreeOLCValue *prev_value, const KeyType *cur_key, BTreeOLCValue *cur_value, const KeyType *next_key, BTreeOLCValue *next_value) -> bool {
                        void *prev_meta = nullptr, *cur_meta = nullptr, *next_meta = nullptr;
                        void *prev_data = nullptr, *cur_data = nullptr, *next_data = nullptr;

                        if (prev_value != nullptr) {
                                const StoredRow row = storage_.Load(&prev_value->row);
                                prev_meta = storage_.AdjacentMeta(row);
                                prev_data = storage_.Data(row);
                        }
                        if (cur_value != nullptr) {
                                removed_row = storage_.Load(&cur_value->row);
                                cur_meta = storage_.AdjacentMeta(removed_row);
                                cur_data = storage_.Data(removed_row);
                        }
                        if (next_value != nullptr) {
                                const StoredRow row = storage_.Load(&next_value->row);
                                next_meta = storage_.AdjacentMeta(row);
                                next_data = storage_.Data(row);
                        }

                        return processor(prev_key, prev_meta, prev_data, cur_key, cur_meta, cur_data, next_key, next_meta, next_data);
		};


                bool success = btree_->remove_and_process_adjacent_keys(k, adjacent_tuples_processor);
                CHECK(success == true);
                if (success && !storage_.IsNull(removed_row))
                        storage_.Retire(removed_row);

                return success;
        }

	void update(const void *key, const void *value, std::function<void(const void *, const void *)> on_update) override
	{
                tid_check();
		const auto &k = *static_cast<const KeyType *>(key);
                BTreeOLCValue btree_value;
                bool success = btree_->lookup(k, btree_value);
                CHECK(success == true);

		const StoredRow row = storage_.Load(&btree_value.row);
		on_update(key, storage_.Data(row));
		storage_.Update(row, value);
	}

        bool search_and_update_next_key_info(const void *key,
                std::function<void(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> update_processor) override
        {
                auto processor = [&](const KeyType *prev_key, BTreeOLCValue *prev_value, const KeyType *cur_key, BTreeOLCValue *cur_value, const KeyType *next_key, BTreeOLCValue *next_value) {
                        void *prev_meta = nullptr, *cur_meta = nullptr, *next_meta = nullptr;
                        void *prev_data = nullptr, *cur_data = nullptr, *next_data = nullptr;
                        if (prev_value != nullptr) {
                                const StoredRow row = storage_.Load(&prev_value->row);
                                prev_meta = storage_.AdjacentMeta(row);
                                prev_data = storage_.Data(row);
                        }
                        if (cur_value != nullptr) {
				const StoredRow row = storage_.Load(&cur_value->row);
				cur_meta = storage_.AdjacentMeta(row);
				cur_data = storage_.Data(row);
                        }
                        if (next_value != nullptr) {
				const StoredRow row = storage_.Load(&next_value->row);
				next_meta = storage_.AdjacentMeta(row);
                                next_data = storage_.Data(row);
                        }
                        update_processor(prev_key, prev_meta, prev_data, cur_key, cur_meta, cur_data, next_key, next_meta, next_data);
		};

                const auto &k = *static_cast<const KeyType *>(key);
                return btree_->lookupForNextKeyUpdate(k, processor);
        }

	void deserialize_value(const void *key, StringPiece stringPiece) override
	{
		tid_check();
		std::size_t size = stringPiece.size();
		const auto &k = *static_cast<const KeyType *>(key);

		BTreeOLCValue value;
                bool success = btree_->lookup(k, value);
                CHECK(success == true);

		storage_.Deserialize(storage_.Load(&value.row), stringPiece);
	}


	void serialize_value(Encoder &enc, const void *value) override
	{
		tid_check();
		storage_.Serialize(enc, value);
	}

	std::size_t key_size() override
	{
		tid_check();
		return storage_.KeySize(sizeof(KeyType));
	}

	std::size_t value_size() override
	{
		tid_check();
		return storage_.ValueSize();
	}

	std::size_t field_size() override
	{
		tid_check();
		return storage_.FieldSize();
	}

	std::size_t tableID() override
	{
		tid_check();
		return tableID_;
	}

	std::size_t partitionID() override
	{
		tid_check();
		return partitionID_;
	}

        int tableType() override
        {
                return BTREE;
        }

        void move_all_into_cxl(std::function<bool(ITable *, const void *, std::tuple<MetaDataType *, void *> &, bool)> move_in_func) override
        {
                auto processor = [&](const KeyType &key, BTreeOLCValue &value, bool) -> bool {
                        const StoredRow row = storage_.Load(&value.row);
                        MetaDataType *meta_ptr = storage_.Meta(row);
                        void *data_ptr = storage_.Data(row);
                        std::tuple<MetaDataType *, void *> row_tuple(meta_ptr, data_ptr);
			bool ret = move_in_func(this, &key, row_tuple, false);
                        return false;
		};

                KeyType start_key{};
                memset(&start_key, 0, sizeof(KeyType));
                btree_->scanForUpdateNoContention(start_key, processor);
        }

        bool lookup_stored_row(const KeyType &key, StoredRow *row) const {
                if (row == nullptr) return false;
                BTreeOLCValue value;
                auto *tree = const_cast<BTree *>(btree_.get());
                if (!tree->lookup(key, value)) return false;
                *row = storage_.Load(&value.row);
                return !storage_.IsNull(*row);
        }

        void bind_published_root(std::atomic<uint64_t> *slot) {
                if (btree_ == nullptr) throw std::runtime_error("BTree is not initialized");
                storage_.BindPublishedRoot(*btree_, slot);
        }

        uint64_t root_offset_for_persistence() const {
                if (btree_ == nullptr) throw std::runtime_error("BTree is not initialized");
                return storage_.RootOffset(*btree_);
        }

    private:
        std::unique_ptr<BTree> btree_;
        RowStoragePolicy storage_;
        std::size_t tableID_;
        std::size_t partitionID_;
};

template <class KeyType, class ValueType, class KeyComparator, class ValueComparator> class HStoreTable : public ITable {
    public:
	using MetaDataType = std::atomic<uint64_t>;

	virtual ~HStoreTable() override = default;

	HStoreTable(std::size_t tableID, std::size_t partitionID)
		: tableID_(tableID)
		, partitionID_(partitionID)
	{
	}

        uint64_t get_plain_key(const void *key) override
        {
                tid_check();
                const auto &k = *static_cast<const KeyType *>(key);
                return k.get_plain_key();
        }

        int compare_key(const void *a, const void *b) override
        {
                const auto &k_a = *static_cast<const KeyType *>(a);
                const auto &k_b = *static_cast<const KeyType *>(b);
                return KeyComparator()(k_a, k_b);
        }

	std::tuple<MetaDataType *, void *> search(const void *key) override
	{
                CHECK(contains(key) == true);
		const auto &k = *static_cast<const KeyType *>(key);
		auto &v = map_[k];
		return std::make_tuple(nullptr, &(v));
	}

	void *search_value(const void *key) override
	{
                CHECK(contains(key) == true);
		const auto &k = *static_cast<const KeyType *>(key);
		return &map_[k];
	}

	MetaDataType *search_metadata(const void *key) override
	{
		static MetaDataType v;
		return &v;
	}

	bool contains(const void *key) override
	{
		const auto &k = *static_cast<const KeyType *>(key);
		return map_.contains(k);
	}

        void scan(const void *min_key, std::function<bool(const void *key, MetaDataType *meta, void *data, bool)> scan_processor) override
        {
                // hash table does not support scan
                CHECK(0);
        }

	bool insert(const void *key, const void *value, bool is_placeholder = false) override
	{
		const auto &k = *static_cast<const KeyType *>(key);
		const auto &v = *static_cast<const ValueType *>(value);
		bool ok = map_.contains(k);
		DCHECK(ok == false);
		auto &row = map_[k];
		row = v;

                return true;
	}

        bool insert_lock_next_key(const void *key, const void *value, std::function<bool(const void *, MetaDataType *, void *)> next_key_processor, bool is_placeholder = false) override
        {
                CHECK(0);
        }

        bool insert_and_process_adjacent_tuples(const void *key, const void *value,
                std::function<bool(const void *prev_key, MetaDataType *prev_meta, void *prev_data, const void *next_key, MetaDataType *next_meta, void *next_data)> update_processor,
                bool is_placeholder = false) override
        {
                CHECK(0);
        }

        bool remove(const void *key) override
        {
                CHECK(0);
        }

        bool remove_and_process_adjacent_tuples(const void *key,
                std::function<bool(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> processor) override
        {
                CHECK(0);
        }

	void update(const void *key, const void *value, std::function<void(const void *, const void *)> on_update) override
	{
		const auto &k = *static_cast<const KeyType *>(key);
		const auto &v = *static_cast<const ValueType *>(value);
		auto &row = map_[k];
		on_update(key, &row);
		row = v;
	}

        bool search_and_update_next_key_info(const void *key,
                std::function<void(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> update_processor) override
        {
                // no need to maintain next-key information for unordered tables
                return true;
        }

	void deserialize_value(const void *key, StringPiece stringPiece) override
	{
		std::size_t size = stringPiece.size();
		const auto &k = *static_cast<const KeyType *>(key);
		auto &row = map_[k];
		auto &v = row;

		Decoder dec(stringPiece);
		dec >> v;

		DCHECK(size - dec.size() == ClassOf<ValueType>::size());
	}

	void serialize_value(Encoder &enc, const void *value) override
	{
		std::size_t size = enc.size();
		const auto &v = *static_cast<const ValueType *>(value);
		enc << v;

		DCHECK(enc.size() - size == ClassOf<ValueType>::size());
	}

	std::size_t key_size() override
	{
		return sizeof(KeyType);
	}

	std::size_t value_size() override
	{
		return sizeof(ValueType);
	}

	std::size_t field_size() override
	{
		return ClassOf<ValueType>::size();
	}

	std::size_t tableID() override
	{
		return tableID_;
	}

	std::size_t partitionID() override
	{
		return partitionID_;
	}

        int tableType() override
        {
                return HASHMAP;
        }

    private:
	UnsafeHashMap<KeyType, ValueType> map_;
	std::size_t tableID_;
	std::size_t partitionID_;
};

template <std::size_t N, class KeyType, class ValueType, class KeyComparator, class ValueComparator> class HStoreCOWTable : public ITable {
    public:
	using MetaDataType = std::atomic<uint64_t>;

	virtual ~HStoreCOWTable() override = default;

	HStoreCOWTable(std::size_t tableID, std::size_t partitionID)
		: tableID_(tableID)
		, partitionID_(partitionID)
	{
	}

        uint64_t get_plain_key(const void *key) override
        {
                tid_check();
                const auto &k = *static_cast<const KeyType *>(key);
                return k.get_plain_key();
        }

        int compare_key(const void *a, const void *b) override
        {
                const auto &k_a = *static_cast<const KeyType *>(a);
                const auto &k_b = *static_cast<const KeyType *>(b);
                return KeyComparator()(k_a, k_b);
        }

	std::tuple<MetaDataType *, void *> search(const void *key) override
	{
                CHECK(contains(key) == true);
		const auto &k = *static_cast<const KeyType *>(key);
		auto &v = map_[k];
		return std::make_tuple(nullptr, &(v));
	}

	void *search_value(const void *key) override
	{
                CHECK(contains(key) == true);
		const auto &k = *static_cast<const KeyType *>(key);
		return &map_[k];
	}

	MetaDataType *search_metadata(const void *key) override
	{
		static MetaDataType v;
		return &v;
	}

	bool contains(const void *key) override
	{
		const auto &k = *static_cast<const KeyType *>(key);
		return map_.contains(k);
	}

        void scan(const void *min_key, std::function<bool(const void *key, MetaDataType *meta, void *data, bool)> scan_processor) override
        {
                // hash table does not support scan
                CHECK(0);
        }

	bool insert(const void *key, const void *value, bool is_placeholder = false) override
	{
		const auto &k = *static_cast<const KeyType *>(key);
		const auto &v = *static_cast<const ValueType *>(value);
		bool ok = map_.contains(k);
		DCHECK(ok == false);
		auto &row = map_[k];
		std::get<1>(row) = v;

                return true;
	}

        bool insert_lock_next_key(const void *key, const void *value, std::function<bool(const void *, MetaDataType *, void *)> next_key_processor, bool is_placeholder = false) override
        {
                CHECK(0);
        }

        bool insert_and_process_adjacent_tuples(const void *key, const void *value,
                std::function<bool(const void *prev_key, MetaDataType *prev_meta, void *prev_data, const void *next_key, MetaDataType *next_meta, void *next_data)> update_processor,
                bool is_placeholder = false) override
        {
                CHECK(0);
        }

        bool remove(const void *key) override
        {
                CHECK(0);
        }

        bool remove_and_process_adjacent_tuples(const void *key,
                std::function<bool(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> processor) override
        {
                CHECK(0);
        }

	void update(const void *key, const void *value, std::function<void(const void *, const void *)> on_update) override
	{
		const auto &k = *static_cast<const KeyType *>(key);
		const auto &v = *static_cast<const ValueType *>(value);
		auto &row = map_[k];
		if (dump_finished == false) {
			if (shadow_map_->contains(k) == false) { // Only store the first version
				auto &shadow_row = (*shadow_map_)[k];
				std::get<1>(shadow_row) = std::get<1>(row);
			}
		}
		on_update(key, &std::get<1>(row));
		std::get<1>(row) = v;
	}

        bool search_and_update_next_key_info(const void *key,
                std::function<void(const void *prev_key, void *prev_meta, void *prev_data, const void *cur_key, void *cur_meta, void *cur_data, const void *next_key, void *next_meta, void *next_data)> update_processor) override
        {
                // no need to maintain next-key information for unordered tables
                return true;
        }

	void deserialize_value(const void *key, StringPiece stringPiece) override
	{
		std::size_t size = stringPiece.size();
		const auto &k = *static_cast<const KeyType *>(key);
		auto &row = map_[k];
		auto &v = std::get<1>(row);
		if (dump_finished == false) {
			if (shadow_map_->contains(k) == false) {
				auto &shadow_row = (*shadow_map_)[k];
				std::get<1>(shadow_row) = std::get<1>(row);
			}
		}
		Decoder dec(stringPiece);
		dec >> v;

		DCHECK(size - dec.size() == ClassOf<ValueType>::size());
	}

	void serialize_value(Encoder &enc, const void *value) override
	{
		std::size_t size = enc.size();
		const auto &v = *static_cast<const ValueType *>(value);
		enc << v;

		DCHECK(enc.size() - size == ClassOf<ValueType>::size());
	}

	std::size_t key_size() override
	{
		return sizeof(KeyType);
	}

	std::size_t value_size() override
	{
		return sizeof(ValueType);
	}

	std::size_t field_size() override
	{
		return ClassOf<ValueType>::size();
	}

	std::size_t tableID() override
	{
		return tableID_;
	}

	std::size_t partitionID() override
	{
		return partitionID_;
	}

        int tableType() override
        {
                return HASHMAP;
        }

	virtual void turn_on_cow() override
	{
		CHECK(cow == false);
		CHECK(shadow_map_ == nullptr);
		shadow_map_ = new HashMap<N, KeyType, std::tuple<MetaDataType, ValueType> >;
		dump_finished.store(false);
		cow.store(true);
	}

	virtual void dump_copy(std::function<void(const void *, const void *)> dump_processor, std::function<void()> dump_unlock) override
	{
		CHECK(cow == true);
		CHECK(dump_finished == false);
		CHECK(shadow_map_ != nullptr);
		auto processor = [&](const KeyType &key, const std::tuple<MetaDataType, ValueType> &row) {
			const auto &v = std::get<1>(row);
			dump_processor((const void *)&key, (const void *)&v);
		};
		map_.iterate(processor, dump_unlock);
		shadow_map_->iterate(processor, dump_unlock);
		dump_finished = true;
		auto clear_COW_status_bits_processor = [&](const KeyType &key, std::tuple<MetaDataType, ValueType> &row) {
			auto &meta = std::get<0>(row);
			meta.store(0);
		};
		map_.iterate_non_const(clear_COW_status_bits_processor, []() {});
	}

	virtual bool cow_dump_finished() override
	{
		return dump_finished;
	}

	virtual std::function<void()> turn_off_cow() override
	{
		CHECK(cow == true);
		auto shadow_map_ptr = shadow_map_;
		auto cleanup_work = [shadow_map_ptr]() { delete shadow_map_ptr; };
		shadow_map_ = nullptr;
		cow.store(false);
		return cleanup_work;
	}

    private:
	HashMap<N, KeyType, std::tuple<MetaDataType, ValueType> > map_;
	HashMap<N, KeyType, std::tuple<MetaDataType, ValueType> > *shadow_map_ = nullptr;
	std::size_t tableID_;
	std::size_t partitionID_;
	std::atomic<bool> dump_finished{ true };
	std::atomic<bool> cow{ false };
};
} // namespace star
