//
// Created by Yibo Huang on 09/02/24 (Labor Day)!
//

#pragma once

#include "stdint.h"

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

struct TwoPLPashaMetadataShared;

class PolicyClock : public MigrationManager {
    public:
        PolicyClock(std::function<migration_result(ITable *, const void *, const std::tuple<std::atomic<uint64_t> *, void *> &, bool, void *&)> move_from_partition_to_shared_region,
                        std::function<bool(ITable *, const void *, const std::tuple<std::atomic<uint64_t> *, void *> &)> move_from_shared_region_to_partition,
                        std::function<bool(ITable *, const void *, bool, bool &, void *&)> delete_and_update_next_key_info,
                        uint64_t coordinator_id,
                        uint64_t partition_num,
                        const std::string when_to_move_out_str,
                        uint64_t hw_cc_budget);

        void init_migration_policy_metadata(void *migration_policy_meta, ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row, uint64_t metadata_size) override;

        void access_row(void *migration_policy_meta, uint64_t partition_id) override;

        migration_result move_row_in(ITable *table, const void *key, const std::tuple<MetaDataType *, void *> &row, bool inc_ref_cnt) override;

        bool move_row_out(uint64_t partition_id) override;

        bool delete_specific_row_and_move_out(ITable *table, const void *key, bool is_delete_local) override;

        void run_under_partition_clock(ITable *table,
                                       const std::function<void()> &fn) override;

        bool try_run_under_partition_clock(
            ITable *table, const std::function<void()> &fn) override;

    private:
        class ClockTracker;
        static tigonkv::engine::KVPartition *PartitionOf(ITable *table);
        static TwoPLPashaMetadataShared *PolicySmeta(void *migration_policy_meta);
        static tigonkv::engine::KVPartition *&HeldClockPartition();

        uint64_t hw_cc_budget{ 0 };
        uint64_t partition_num_{ 0 };
};

} // namespace star
