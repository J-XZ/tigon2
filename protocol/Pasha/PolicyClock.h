//
// Created by Yibo Huang on 09/02/24 (Labor Day)!
//

#pragma once

#include "stdint.h"

#include <atomic>
#include <functional>
#include <string>
#include <tuple>

#include "common/CXLMemory.h"
#include "core/Table.h"
#include "kv/engine/mem_access.h"
#include "protocol/Pasha/MigrationManager.h"

namespace tigonkv::engine {
class KVPartition;
class KvPartitionTable;
}

namespace star
{

class PolicyClock : public MigrationManager {
    public:
        struct ClockMeta {
                // A fresh move-in has not yet been accessed.  Preserve the
                // original Clock initial state; access_row is the sole path
                // that grants a second chance.
                std::atomic<uint8_t> second_chance{0};
        };

        PolicyClock(std::function<migration_result(ITable *, const void *, const std::tuple<std::atomic<uint64_t> *, void *> &, bool, void *&)> move_from_partition_to_shared_region,
                        std::function<bool(ITable *, const void *, const std::tuple<std::atomic<uint64_t> *, void *> &)> move_from_shared_region_to_partition,
                        std::function<bool(ITable *, const void *, bool, bool &, void *&)> delete_and_update_next_key_info,
                        uint64_t coordinator_id,
                        uint64_t partition_num,
                        const std::string when_to_move_out_str,
                        uint64_t hw_cc_budget);

        void init_migration_policy_metadata(void *migration_policy_meta, ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row, uint64_t metadata_size) override;

        void access_row(void *migration_policy_meta, uint64_t partition_id) override;

        bool move_specific_row_out(ITable *table, const void *key);

        migration_result move_row_in(ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row, bool inc_ref_cnt) override;

        bool move_row_out(uint64_t partition_id) override;

        bool delete_specific_row_and_move_out(ITable *table, const void *key, bool is_delete_local) override;

    private:
        static tigonkv::engine::KVPartition *PartitionOf(ITable *table);

        uint64_t hw_cc_budget{ 0 };
        uint64_t partition_num_{ 0 };
        std::tuple<MetaDataType *, void *> empty_row_{nullptr, nullptr};
};

} // namespace star
