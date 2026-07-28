//
// Created by Yi Lu on 9/11/18.
//

#pragma once
#include <unordered_set>
#include "common/Encoder.h"
#include "common/Message.h"
#include "common/MessagePiece.h"
#include "core/ControlMessage.h"
#include "core/Table.h"

#include "protocol/TwoPLPasha/TwoPLPashaHelper.h"
#include "protocol/TwoPLPasha/TwoPLPashaRWKey.h"
#include "protocol/TwoPLPasha/TwoPLPashaTransaction.h"

namespace star
{

enum class TwoPLPashaMessage {
	DATA_MIGRATION_REQUEST = static_cast<int>(ControlMessage::NFIELDS),
        DATA_MIGRATION_RESPONSE,
        DATA_MIGRATION_REQUEST_FOR_SCAN,
        DATA_MIGRATION_RESPONSE_FOR_SCAN,
        DATA_MOVEOUT_HINT,
        REMOTE_INSERT_REQUEST,
        REMOTE_INSERT_RESPONSE,
        REMOTE_DELETE_REQUEST,
        // The original remote delete is one-way because transaction cleanup
        // owns completion.  The synchronous KV facade needs only this empty
        // completion marker; it carries no new status framing.
        REMOTE_DELETE_RESPONSE,
        REPLICATION_REQUEST,
	REPLICATION_RESPONSE,
	NFIELDS
};

class TwoPLPashaMessageFactory {
    public:
        static std::size_t new_data_migration_message(Message &message, ITable &table, const void *key, uint64_t transaction_id, uint32_t key_offset)
	{
		/*
		 * The structure of a data migration request: (primary key, transaction_id, key_offset)
		 */

		auto key_size = table.key_size();

		auto message_size = MessagePiece::get_header_size() + key_size + sizeof(transaction_id) + sizeof(key_offset);
		auto message_piece_header = MessagePiece::construct_message_piece_header(static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_REQUEST),
                                                                                         message_size, table.tableID(), table.partitionID());

		Encoder encoder(message.data);
		encoder << message_piece_header;
		encoder.write_n_bytes(key, key_size);
		encoder << transaction_id;
		encoder << key_offset;
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}

        static std::size_t new_data_migration_message_for_scan(Message &message, ITable &table, const void *min_key, const void *max_key, uint64_t limit, uint64_t transaction_id, uint32_t key_offset)
	{
		/*
		 * The structure of a data migration request: (min_key, max_key, limit, transaction_id, key_offset)
		 */

		auto key_size = table.key_size();

		auto message_size = MessagePiece::get_header_size() + key_size + key_size + sizeof(limit) + sizeof(transaction_id) + sizeof(key_offset);
		auto message_piece_header = MessagePiece::construct_message_piece_header(static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN),
                                                                                         message_size, table.tableID(), table.partitionID());

		Encoder encoder(message.data);
		encoder << message_piece_header;
		encoder.write_n_bytes(min_key, key_size);
                encoder.write_n_bytes(max_key, key_size);
                encoder << limit;
		encoder << transaction_id;
		encoder << key_offset;
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}

        static std::size_t new_data_move_out_hint_message(Message &message)
	{
		/*
		 * The structure of a data move out hint: ()
		 */
		auto message_size = MessagePiece::get_header_size();
		auto message_piece_header = MessagePiece::construct_message_piece_header(static_cast<uint32_t>(TwoPLPashaMessage::DATA_MOVEOUT_HINT),
                                                                                         message_size, 0, 0);

		Encoder encoder(message.data);
		encoder << message_piece_header;
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}

	static std::size_t new_replication_message(Message &message, ITable &table, const void *key, const void *value, uint64_t commit_tid, bool sync_redo)
	{
		CHECK(0);
	}

        static std::size_t new_remote_insert_message(Message &message, ITable &table, const void *key, void *value, uint64_t transaction_id, uint32_t key_offset)
	{
		/*
		 * The structure of a remote delete request: (primary key, value)
		 */

		auto key_size = table.key_size();
                auto value_size = table.value_size();

		auto message_size = MessagePiece::get_header_size() + key_size + value_size + sizeof(transaction_id) + sizeof(key_offset);
		auto message_piece_header = MessagePiece::construct_message_piece_header(static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_INSERT_REQUEST),
                                                                                         message_size, table.tableID(), table.partitionID());

		Encoder encoder(message.data);
		encoder << message_piece_header;
		encoder.write_n_bytes(key, key_size);
                encoder.write_n_bytes(value, value_size);
                encoder << transaction_id;
		encoder << key_offset;
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}

        static std::size_t new_remote_delete_message(Message &message, ITable &table, const void *key)
	{
		/*
		 * The structure of a remote delete request: (primary key)
		 */

		auto key_size = table.key_size();

		auto message_size = MessagePiece::get_header_size() + key_size;
		auto message_piece_header = MessagePiece::construct_message_piece_header(static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_DELETE_REQUEST),
                                                                                         message_size, table.tableID(), table.partitionID());

		Encoder encoder(message.data);
		encoder << message_piece_header;
		encoder.write_n_bytes(key, key_size);
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}
};

class TwoPLPashaMessageHandler {
	using Transaction = TwoPLPashaTransaction;

    public:
        // The owner half of the original scan migration request.  Keeping it
        // here lets the transaction handler and the transaction-free KV
        // facade share the exact ITable scan / move_row_in sequence instead
        // of growing a second range-paging protocol in KVPartition.
        static void move_in_scan_range(ITable &table, const void *min_key,
                                       const void *max_key, uint64_t limit)
        {
                std::vector<ITable::row_entity> scan_results;
                auto scan_processor = [&](const void *key,
                                          std::atomic<uint64_t> *meta_ptr,
                                          void *data_ptr,
                                          bool is_last_tuple) -> bool {
                        (void)is_last_tuple;
                        DCHECK(key != nullptr);
                        DCHECK(meta_ptr != nullptr);
                        DCHECK(data_ptr != nullptr);

                        bool migrating_next_key = false;
                        if (limit != 0 && scan_results.size() == limit) {
                                migrating_next_key = true;
                        } else if (table.compare_key(key, max_key) > 0) {
                                migrating_next_key = true;
                        }

                        if (table.compare_key(key, min_key) >= 0) {
                                if (scan_results.size() > 0 &&
                                    table.compare_key(
                                        key,
                                        scan_results[scan_results.size() - 1].key) <= 0) {
                                        return false;
                                }
                        } else {
                                return false;
                        }

                        // The original handler deliberately ignores a race
                        // between this scan and move_row_in.  The requester
                        // re-runs the same CXL scan after the bool response.
                        scan_results.emplace_back(key, table.key_size(), meta_ptr,
                                                  data_ptr, table.value_size());
                        return migrating_next_key;
                };
                table.scan(min_key, scan_processor);

                for (auto &row : scan_results) {
                        std::tuple<std::atomic<uint64_t> *, void *> row_tuple(
                            row.meta, row.data);
                        // Same original semantics: a missing/deleted row or
                        // allocation failure is handled by the re-probe, and
                        // this range migration never obtains requester refs.
                        migration_manager->move_row_in(&table, row.key, row_tuple,
                                                       false);
                }
        }

        static void data_migration_request_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(inputPiece.get_message_type() == static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_REQUEST));
		auto table_id = inputPiece.get_table_id();
		auto partition_id = inputPiece.get_partition_id();
		DCHECK(table_id == table.tableID());
		DCHECK(partition_id == table.partitionID());
		auto key_size = table.key_size();
		auto value_size = table.value_size();

		/*
		 * The structure of a data migration request: (key, transaction_id, key_offset)
		 * The structure of a data migration response: (success, key_offset)
		 */

		auto stringPiece = inputPiece.toStringPiece();
		uint32_t key_offset;
		uint64_t transaction_id;
                bool success = false;

		DCHECK(inputPiece.get_message_length() ==
		       MessagePiece::get_header_size() + key_size + sizeof(transaction_id) + sizeof(key_offset));

		// get row and offset
		const void *key = stringPiece.data();
		auto row = table.search(key);

		stringPiece.remove_prefix(key_size);
		star::Decoder dec(stringPiece);
		dec >> transaction_id >> key_offset;

		DCHECK(dec.size() == 0);

                // move the tuple to the shared region if it is not currently there
                // the return value does not matter
                migration_result res = migration_manager->move_row_in(&table, key, row, false);
                if (res == migration_result::FAIL_OOM) {
                        success = false;
                } else {
                        success = true;
                }

		// prepare response message header
		auto message_size = MessagePiece::get_header_size() + sizeof(success) + sizeof(key_offset);
		auto message_piece_header = MessagePiece::construct_message_piece_header(static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_RESPONSE), message_size,
                                                                                         table_id, partition_id);

		star::Encoder encoder(responseMessage.data);
		encoder << message_piece_header;
                encoder << success << key_offset;
		responseMessage.flush();

                if (migration_manager->when_to_move_out == MigrationManager::OnDemand) {
                        // after moving in the tuple, we move out tuples
                        migration_manager->move_row_out(table.partitionID());
                }
	}

	static void data_migration_response_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(inputPiece.get_message_type() == static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_RESPONSE));
		auto table_id = inputPiece.get_table_id();
		auto partition_id = inputPiece.get_partition_id();
		DCHECK(table_id == table.tableID());
		DCHECK(partition_id == table.partitionID());
		auto key_size = table.key_size();
		auto value_size = table.value_size();

		/*
		 * The structure of a data migration request: (key, transaction_id, key_offset)
		 * The structure of a data migration response: (success, key_offset)
		 */

		auto stringPiece = inputPiece.toStringPiece();
		uint32_t key_offset;
		bool success;

		DCHECK(inputPiece.get_message_length() ==
		       MessagePiece::get_header_size() + sizeof(success) + sizeof(key_offset));

		Decoder dec(stringPiece);
		dec >> success >> key_offset;

                if (success == false) {
                        txn->abort_lock = true;
                        txn->pendingResponses--;
                        return;
                }

		TwoPLPashaRWKey &readKey = txn->readSet[key_offset];
                uint64_t tid = 0;

                // search cxl table and get the data
                char *migrated_row = twopl_pasha_global_helper->get_migrated_row(table_id, partition_id, readKey.get_key(), false);
                if (migrated_row == nullptr) {
                        txn->abort_lock = true;
                } else {
                        // perform execution phase operations
                        if (readKey.get_write_lock_request_bit()) {
                                tid = twopl_pasha_global_helper->remote_take_write_lock_and_read(migrated_row, readKey.get_value(), value_size, true, success);
                        } else {
                                tid = twopl_pasha_global_helper->remote_take_read_lock_and_read(migrated_row, readKey.get_value(), value_size, true, success);
                        }

                        if (success) {
                                readKey.set_tid(tid);
                                readKey.set_cached_migrated_row(migrated_row);
                                DCHECK(readKey.get_local_index_read_bit() == 0);
                                if (readKey.get_read_lock_request_bit()) {
                                        readKey.set_read_lock_bit();
                                }
                                if (readKey.get_write_lock_request_bit()) {
                                        readKey.set_write_lock_bit();
                                }

                                // mark it as reference counted so that we know if we need to release it upon commit/abort
                                readKey.set_reference_counted();
                        } else {
                                txn->abort_lock = true;
                        }
                }

                txn->pendingResponses--;
	}

        static void data_migration_request_for_scan_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(inputPiece.get_message_type() == static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN));
		auto table_id = inputPiece.get_table_id();
		auto partition_id = inputPiece.get_partition_id();
		DCHECK(table_id == table.tableID());
		DCHECK(partition_id == table.partitionID());
		auto key_size = table.key_size();
		auto value_size = table.value_size();

		/*
		 * The structure of a data migration request: (min_key, max_key, limit, transaction_id, key_offset)
		 * The structure of a data migration response: (success, key_offset)
		 */

		auto stringPiece = inputPiece.toStringPiece();
                uint64_t limit;
		uint64_t transaction_id;
		uint32_t key_offset;
                bool success = false;

		DCHECK(inputPiece.get_message_length() ==
		       MessagePiece::get_header_size() + key_size + key_size +
                       sizeof(limit) + sizeof(transaction_id) + sizeof(key_offset));

		// get min_key
		const void *min_key = stringPiece.data();
		stringPiece.remove_prefix(key_size);

                // get max_key
                const void *max_key = stringPiece.data();
		stringPiece.remove_prefix(key_size);

                // get limit, transaction_id, and key_offset
		star::Decoder dec(stringPiece);
		dec >> limit >> transaction_id >> key_offset;

		DCHECK(dec.size() == 0);

                move_in_scan_range(table, min_key, max_key, limit);
                success = true;

		// prepare response message header
		auto message_size = MessagePiece::get_header_size() + sizeof(success) + sizeof(key_offset);
		auto message_piece_header = MessagePiece::construct_message_piece_header(static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN), message_size,
                                                                                         table_id, partition_id);

		star::Encoder encoder(responseMessage.data);
		encoder << message_piece_header;
                encoder << success << key_offset;
		responseMessage.flush();

                if (migration_manager->when_to_move_out == MigrationManager::OnDemand) {
                        // after moving in the tuple, we move out tuples
                        migration_manager->move_row_out(table.partitionID());
                }
	}

        static void data_migration_response_for_scan_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(inputPiece.get_message_type() == static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN));
		auto table_id = inputPiece.get_table_id();
		auto partition_id = inputPiece.get_partition_id();
		DCHECK(table_id == table.tableID());
		DCHECK(partition_id == table.partitionID());
		auto key_size = table.key_size();
		auto value_size = table.value_size();

		/*
		 * The structure of a data migration request: (key, transaction_id, key_offset)
		 * The structure of a data migration response: (success, key_offset)
		 */

		auto stringPiece = inputPiece.toStringPiece();
		uint32_t key_offset;
		bool success;

		DCHECK(inputPiece.get_message_length() ==
		       MessagePiece::get_header_size() + sizeof(success) + sizeof(key_offset));

		Decoder dec(stringPiece);
		dec >> success >> key_offset;
                DCHECK(success == true);

		TwoPLPashaRWKey &scanKey = txn->scanSet[key_offset];
                uint64_t tid = 0;

                const void *min_key = scanKey.get_scan_min_key();
                const void *max_key = scanKey.get_scan_max_key();
                uint64_t limit = scanKey.get_scan_limit();
                int type = scanKey.get_request_type();
                std::vector<ITable::row_entity> &scan_results = *reinterpret_cast<std::vector<ITable::row_entity> *>(scanKey.get_scan_res_vec());

                // we do the next-key locking logic inside this function
                bool scan_success = false;       // it is possible that the range is empty - we return fail and abort in this case
                bool migration_required = false;
                auto remote_scan_processor = [&](const void *key, void *cxl_row, bool is_last_tuple) -> bool {
                        DCHECK(key != nullptr);
                        DCHECK(cxl_row != nullptr);
                        DCHECK(scan_results.size() <= limit);

                        bool locking_next_tuple = false;

                        if (is_last_tuple == true) {
                                locking_next_tuple = true;
                        } else if (limit != 0 && scan_results.size() == limit) {
                                locking_next_tuple = true;
                        } else if (table.compare_key(key, max_key) > 0) {
                                locking_next_tuple = true;
                        }

                        if (table.compare_key(key, min_key) >= 0) {
                                if (scan_results.size() > 0) {
                                        if (table.compare_key(key, scan_results[scan_results.size() - 1].key) <= 0) {
                                                return false;
                                        }
                                }
                        } else {
                                return false;
                        }

                        // check if the previous key and the next key are real
                        TwoPLPashaMetadataShared *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cxl_row);
                        TwoPLPashaSharedDataSCC *scc_data = smeta->get_scc_data();

                        smeta->lock();
                        if (!TwoPLPashaHelper::scan_row_adjacency_ok(
                                table.compare_key(key, min_key) == 0,
                                scan_results.size() == limit,
                                smeta->get_prev_key_real_bit(),
                                smeta->get_next_key_real_bit())) {
                                migration_required = true;
                        }
                        smeta->unlock();

                        // stop immediately if data migration is needed
                        if (migration_required == true) {
                                scan_success = false;
                                return true;
                        }

                        // TODO: Theoretically, we need to check if the current tuple is already locked by previous queries.
                        // But we can ignore it for now because we never generate
                        // transactions with duplicated or overlapped queries.

                        // try to acquire the lock and increase the reference count if locking succeeds
                        bool lock_success = false;
                        if (type == TwoPLPashaRWKey::SCAN_FOR_READ) {
                                twopl_pasha_global_helper->remote_read_lock_and_inc_ref_cnt(reinterpret_cast<char *>(cxl_row), table.value_size(), lock_success);
                        } else if (type == TwoPLPashaRWKey::SCAN_FOR_UPDATE) {
                                twopl_pasha_global_helper->remote_write_lock_and_inc_ref_cnt(reinterpret_cast<char *>(cxl_row), table.value_size(), lock_success);
                        } else if (type == TwoPLPashaRWKey::SCAN_FOR_INSERT) {
                                twopl_pasha_global_helper->remote_write_lock_and_inc_ref_cnt(reinterpret_cast<char *>(cxl_row), table.value_size(), lock_success);
                        } else if (type == TwoPLPashaRWKey::SCAN_FOR_DELETE) {
                                twopl_pasha_global_helper->remote_write_lock_and_inc_ref_cnt(reinterpret_cast<char *>(cxl_row), table.value_size(), lock_success);
                        } else {
                                DCHECK(0);
                        }

                        if (lock_success == true) {
                                // acquiring lock succeeds
                                ITable::row_entity cur_row(key, table.key_size(), reinterpret_cast<std::atomic<uint64_t> *>(cxl_row), scc_data->data, table.value_size());
                                if (locking_next_tuple == false) {
                                        scan_results.push_back(cur_row);
                                        // continue scan
                                        return false;
                                } else {
                                        // scan succeeds - store the next-tuple and quit
                                        scanKey.set_next_row_entity(cur_row);
                                        scanKey.set_next_row_locked();
                                        scan_success = true;
                                        return true;
                                }
                        } else {
                                // stop and fail immediately if we fail to acquire a lock
                                scan_success = false;
                                return true;
                        }
                };

                CXLTableBase *target_cxl_table = twopl_pasha_global_helper->get_cxl_table(table_id, partition_id);
                target_cxl_table->scan(min_key, remote_scan_processor);

                if (migration_required == true) {
                        // if race condition happens, we abort and try again later
                        // race condition example: the row is moved out before we access it
                        DCHECK(scan_success == false);
                        txn->abort_lock = true;
                } else {
                        if (scan_success == false) {
                                txn->abort_lock = true;
                        } else {
                                // remote scan succeed!
                                // do nothing
                        }
                }

                // mark it as reference counted so that we know if we need to release it upon commit/abort
                scanKey.set_reference_counted();

                txn->pendingResponses--;
	}

        static void data_move_out_hint_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
                migration_manager->move_row_out(table.partitionID());
	}

        static void remote_insert_request_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(inputPiece.get_message_type() == static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_INSERT_REQUEST));
		auto table_id = inputPiece.get_table_id();
		auto partition_id = inputPiece.get_partition_id();
		DCHECK(table_id == table.tableID());
		DCHECK(partition_id == table.partitionID());
		auto key_size = table.key_size();
		auto value_size = table.value_size();

		/*
		 * The structure of a remote insert request: (primary key, value)
		 */

		auto stringPiece = inputPiece.toStringPiece();
		uint64_t transaction_id;
		uint32_t key_offset;

		DCHECK(inputPiece.get_message_length() == MessagePiece::get_header_size() + key_size + value_size + sizeof(transaction_id) + sizeof(key_offset));

                // get the key
		const void *key = stringPiece.data();
		stringPiece.remove_prefix(key_size);

                // get the value
                const void *value = stringPiece.data();
		stringPiece.remove_prefix(value_size);

                // get transaction_id, and key_offset
		star::Decoder dec(stringPiece);
		dec >> transaction_id >> key_offset;

		DCHECK(dec.size() == 0);

                // insert a placeholder
                ITable::row_entity next_row_entity;
                bool insert_success = twopl_pasha_global_helper->insert_and_update_next_key_info(&table, key, value, false, next_row_entity);
                DCHECK(insert_success == true);

                // move it into CXL memory
                auto row = table.search(key);
                migration_manager->move_row_in(&table, key, row, true);

                // prepare response message header
		auto message_size = MessagePiece::get_header_size() + sizeof(insert_success) + sizeof(key_offset);
		auto message_piece_header = MessagePiece::construct_message_piece_header(static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_INSERT_RESPONSE), message_size,
                                                                                         table_id, partition_id);

		star::Encoder encoder(responseMessage.data);
		encoder << message_piece_header;
                encoder << insert_success << key_offset;
		responseMessage.flush();
	}

        static void remote_insert_response_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(inputPiece.get_message_type() == static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_DELETE_REQUEST));
		auto table_id = inputPiece.get_table_id();
		auto partition_id = inputPiece.get_partition_id();
		DCHECK(table_id == table.tableID());
		DCHECK(partition_id == table.partitionID());
		auto key_size = table.key_size();
		auto value_size = table.value_size();

		/*
		 * The structure of a remote insert response: (success, key_offset)
		 */

		auto stringPiece = inputPiece.toStringPiece();
                bool success;
                uint32_t key_offset;

		DCHECK(inputPiece.get_message_length() == MessagePiece::get_header_size() + sizeof(success) + sizeof(key_offset));

		// get success and key_offset
		star::Decoder dec(stringPiece);
		dec >> success >> key_offset;

                // always succeeds
                DCHECK(success == true);

                TwoPLPashaRWKey &insertKey = txn->insertSet[key_offset];
                DCHECK(insertKey.get_processed() == true);

                // mark the placeholder as valid
                auto key = insertKey.get_key();
                CXLTableBase *target_cxl_table = twopl_pasha_global_helper->get_cxl_table(table_id, partition_id);
                char *cxl_row = reinterpret_cast<char *>(target_cxl_table->search(key));
                twopl_pasha_global_helper->remote_modify_tuple_valid_bit(cxl_row, true, true);

                // record it locally
                insertKey.set_inserted_cxl_row(cxl_row);

                // mark it as reference counted so that we know if we need to release it upon commit/abort
                insertKey.set_reference_counted();

                txn->pendingResponses--;
	}

        static void remote_delete_request_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(inputPiece.get_message_type() == static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_DELETE_REQUEST));
		auto table_id = inputPiece.get_table_id();
		auto partition_id = inputPiece.get_partition_id();
		DCHECK(table_id == table.tableID());
		DCHECK(partition_id == table.partitionID());
		auto key_size = table.key_size();
		auto value_size = table.value_size();

		/*
		 * The structure of a remote delete request: (primary key)
		 */

		auto stringPiece = inputPiece.toStringPiece();

		DCHECK(inputPiece.get_message_length() == MessagePiece::get_header_size() + key_size);

		// get the key
		const void *key = stringPiece.data();

                // delete the key and untrack it if necessary
                migration_manager->delete_specific_row_and_move_out(&table, key, false);
	}

	static void replication_request_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(0);
	}

	static void replication_response_handler(MessagePiece inputPiece, Message &responseMessage, ITable &table, Transaction *txn)
	{
		DCHECK(0);
	}

    public:
	static std::vector<std::function<void(MessagePiece, Message &, ITable &, Transaction *)> > get_message_handlers()
	{
		std::vector<std::function<void(MessagePiece, Message &, ITable &, Transaction *)> > v;
		v.resize(static_cast<int>(ControlMessage::NFIELDS));
		v.push_back(data_migration_request_handler);
		v.push_back(data_migration_response_handler);
		v.push_back(data_migration_request_for_scan_handler);
		v.push_back(data_migration_response_for_scan_handler);
                v.push_back(data_move_out_hint_handler);
                v.push_back(remote_insert_request_handler);
                v.push_back(remote_insert_response_handler);
                v.push_back(remote_delete_request_handler);
                // replication is not supported
                // v.push_back(replication_request_handler);
		// v.push_back(replication_response_handler);
		return v;
	}
};

} // namespace star
