//
// Created by Yi Lu on 9/11/18.
//

#pragma once
#include <cstdint>
#include <functional>
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
        // The transaction-free KV facade uses the response to distinguish
        // owner-side completion from retryable delete contention.
        REMOTE_DELETE_RESPONSE,
        REPLICATION_REQUEST,
	REPLICATION_RESPONSE,
	NFIELDS
};

enum class RemoteInsertOutcome : uint8_t {
        Inserted = 0,
        AlreadyExists = 1,
        Busy = 2,
        NoMemory = 3,
};

// A remote delete is a request/response operation in the transaction-free KV
// facade.  The requester has already published an invalid shared row before
// sending the request, so a false owner-side delete is retryable contention,
// not a malformed transport frame.
enum class RemoteDeleteOutcome : uint8_t {
        Deleted = 0,
        Busy = 1,
        Failed = 2,
};

enum class MigrationResponseOutcome : uint8_t {
        Migrated = 0,
        Missing = 1,
        Busy = 2,
        NoMemory = 3,
};

class TwoPLPashaMessageFactory {
    public:
        static std::size_t data_migration_request_size(std::size_t key_size) {
                return MessagePiece::get_header_size() + key_size +
                       sizeof(uint64_t) + sizeof(uint32_t);
        }
        static std::size_t scan_migration_request_size(std::size_t key_size) {
                return MessagePiece::get_header_size() + 2 * key_size +
                       sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t);
        }
        static std::size_t remote_insert_request_size(
                std::size_t key_size, std::size_t value_size) {
                return MessagePiece::get_header_size() + key_size + value_size +
                       sizeof(uint64_t) + sizeof(uint32_t);
        }
        static std::size_t remote_delete_request_size(std::size_t key_size) {
                return MessagePiece::get_header_size() + key_size +
                       sizeof(uint64_t) + sizeof(uint64_t);
        }
        static std::size_t remote_delete_response_size() {
                return MessagePiece::get_header_size() + sizeof(uint8_t) +
                       sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t);
        }
        static std::size_t bool_key_offset_response_size() {
                return MessagePiece::get_header_size() + sizeof(bool) +
                       sizeof(uint32_t);
        }
        static std::size_t status_key_offset_response_size() {
                return MessagePiece::get_header_size() + sizeof(uint8_t) +
                       sizeof(uint32_t);
        }
        static std::size_t empty_response_size() {
                return MessagePiece::get_header_size();
        }

        static std::size_t new_data_migration_message(
                Message &message, std::size_t table_id, std::size_t partition_id,
                const void *key, std::size_t key_size, uint64_t transaction_id,
                uint32_t key_offset)
	{
		auto message_size = data_migration_request_size(key_size);
		auto message_piece_header = MessagePiece::construct_message_piece_header(
		    static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_REQUEST),
		    message_size, table_id, partition_id);
		Encoder encoder(message.data);
		encoder << message_piece_header;
		encoder.write_n_bytes(key, key_size);
		encoder << transaction_id << key_offset;
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}

        static std::size_t new_data_migration_message(Message &message, ITable &table, const void *key, uint64_t transaction_id, uint32_t key_offset)
	{
		return new_data_migration_message(message, table.tableID(),
		    table.partitionID(), key, table.key_size(), transaction_id,
		    key_offset);
	}

        static std::size_t new_data_migration_message_for_scan(
                Message &message, std::size_t table_id, std::size_t partition_id,
                const void *min_key, const void *max_key, std::size_t key_size,
                uint64_t limit, uint64_t transaction_id, uint32_t key_offset)
	{
		auto message_size = scan_migration_request_size(key_size);
		auto message_piece_header = MessagePiece::construct_message_piece_header(
		    static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN),
		    message_size, table_id, partition_id);
		Encoder encoder(message.data);
		encoder << message_piece_header;
		encoder.write_n_bytes(min_key, key_size);
		encoder.write_n_bytes(max_key, key_size);
		encoder << limit << transaction_id << key_offset;
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}

        static std::size_t new_data_migration_message_for_scan(Message &message, ITable &table, const void *min_key, const void *max_key, uint64_t limit, uint64_t transaction_id, uint32_t key_offset)
	{
		return new_data_migration_message_for_scan(
		    message, table.tableID(), table.partitionID(), min_key, max_key,
		    table.key_size(), limit, transaction_id, key_offset);
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

        static std::size_t new_remote_insert_message(
                Message &message, std::size_t table_id, std::size_t partition_id,
                const void *key, std::size_t key_size, const void *value,
                std::size_t value_size, uint64_t transaction_id,
                uint32_t key_offset)
	{
		auto message_size = remote_insert_request_size(key_size, value_size);
		auto message_piece_header = MessagePiece::construct_message_piece_header(
		    static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_INSERT_REQUEST),
		    message_size, table_id, partition_id);
		Encoder encoder(message.data);
		encoder << message_piece_header;
		encoder.write_n_bytes(key, key_size);
		encoder.write_n_bytes(value, value_size);
		encoder << transaction_id << key_offset;
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}

        static std::size_t new_remote_insert_message(Message &message, ITable &table, const void *key, void *value, uint64_t transaction_id, uint32_t key_offset)
	{
		return new_remote_insert_message(
		    message, table.tableID(), table.partitionID(), key, table.key_size(),
		    value, table.value_size(), transaction_id, key_offset);
	}

        static std::size_t new_remote_delete_message(
                Message &message, std::size_t table_id, std::size_t partition_id,
                const void *key, std::size_t key_size, uint64_t target_row,
                uint64_t request_sequence)
	{
		auto message_size = remote_delete_request_size(key_size);
		auto message_piece_header = MessagePiece::construct_message_piece_header(
		    static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_DELETE_REQUEST),
		    message_size, table_id, partition_id);
		Encoder encoder(message.data);
		encoder << message_piece_header;
		encoder.write_n_bytes(key, key_size);
		encoder << target_row << request_sequence;
		message.flush();
		message.set_gen_time(Time::now());
		return message_size;
	}

};

class TwoPLPashaMessageHandler {
	using Transaction = TwoPLPashaTransaction;

    public:
        // Transaction-free request overloads keep the original decode,
        // row-action, response, and post-response ordering in this handler.
        // The facade supplies only its existing owner binding and operation
        // callback; it does not own another wire state machine.
        static bool data_migration_request_handler(
                MessagePiece inputPiece, Message &responseMessage, ITable &table,
                std::size_t key_size,
                const std::function<MigrationResponseOutcome(const void *)> &move_row_in,
                const std::function<void()> &after_response = {})
        {
                const char *key = nullptr;
                uint64_t transaction_id = 0;
                uint32_t key_offset = 0;
                if (!decode_data_migration_request(
                        inputPiece, key_size, key, transaction_id, key_offset) ||
                    key_offset != 0)
                        return false;
                (void)transaction_id;
                const MigrationResponseOutcome outcome = move_row_in(key);
                append_data_migration_response(responseMessage, table.tableID(),
                    table.partitionID(), outcome, key_offset);
                if (outcome == MigrationResponseOutcome::Migrated && after_response)
                        after_response();
                return true;
        }

        static bool remote_insert_request_handler(
                MessagePiece inputPiece, Message &responseMessage, ITable &table,
                std::size_t key_size, std::size_t value_size,
                const std::function<RemoteInsertOutcome(const void *, const void *)> &insert_row)
        {
                const char *key = nullptr;
                const char *value = nullptr;
                uint64_t transaction_id = 0;
                uint32_t key_offset = 0;
                if (!decode_remote_insert_request(inputPiece, key_size, value_size,
                                                   key, value, transaction_id,
                                                   key_offset) || key_offset != 0)
                        return false;
                (void)transaction_id;
                const RemoteInsertOutcome outcome = insert_row(key, value);
                append_remote_insert_response(responseMessage, table.tableID(),
                    table.partitionID(), outcome, key_offset);
                return true;
        }

        static bool remote_delete_request_handler(
                MessagePiece inputPiece, Message &responseMessage, ITable &table,
                std::size_t key_size,
                const std::function<RemoteDeleteOutcome(const void *, uint64_t,
                                                         uint64_t)> &delete_row)
        {
                const char *key = nullptr;
                uint64_t target_row = 0;
                uint64_t request_sequence = 0;
                if (!decode_remote_delete_request(inputPiece, key_size, key,
                                                  target_row, request_sequence))
                        return false;
                const RemoteDeleteOutcome outcome =
                    delete_row(key, target_row, request_sequence);
                append_remote_delete_response(responseMessage, table.tableID(),
                    table.partitionID(), outcome, /*key_offset=*/0,
                    target_row, request_sequence);
                return true;
        }

        static bool data_migration_request_for_scan_handler(
                MessagePiece inputPiece, Message &responseMessage, ITable &table,
                std::size_t key_size,
                const std::function<bool(const void *, const void *, uint64_t)> &
                    move_range,
                const std::function<void()> &after_response = {})
        {
                const char *min_key = nullptr;
                const char *max_key = nullptr;
                uint64_t limit = 0;
                uint64_t transaction_id = 0;
                uint32_t key_offset = 0;
                if (!decode_scan_migration_request(
                        inputPiece, key_size, min_key, max_key, limit,
                        transaction_id, key_offset) || key_offset != 0)
                        return false;
                (void)transaction_id;
                const bool success = move_range(min_key, max_key, limit);
                append_scan_migration_response(responseMessage, table.tableID(),
                    table.partitionID(), success, key_offset);
                if (success && after_response) after_response();
                return true;
        }


        static bool decode_data_migration_request(
                MessagePiece piece, std::size_t key_size, const char *&key,
                uint64_t &transaction_id, uint32_t &key_offset)
        {
                if (piece.get_message_type() != static_cast<uint32_t>(
                            TwoPLPashaMessage::DATA_MIGRATION_REQUEST) ||
                    piece.get_message_length() !=
                        TwoPLPashaMessageFactory::data_migration_request_size(key_size))
                        return false;
                auto input = piece.toStringPiece();
                key = input.data();
                input.remove_prefix(key_size);
                Decoder decoder(input);
                decoder >> transaction_id >> key_offset;
                return decoder.size() == 0;
        }

        static bool decode_remote_insert_request(
                MessagePiece piece, std::size_t key_size, std::size_t value_size,
                const char *&key, const char *&value,
                uint64_t &transaction_id, uint32_t &key_offset)
        {
                if (piece.get_message_type() != static_cast<uint32_t>(
                            TwoPLPashaMessage::REMOTE_INSERT_REQUEST) ||
                    piece.get_message_length() !=
                        TwoPLPashaMessageFactory::remote_insert_request_size(
                            key_size, value_size))
                        return false;
                auto input = piece.toStringPiece();
                key = input.data();
                input.remove_prefix(key_size);
                value = input.data();
                input.remove_prefix(value_size);
                Decoder decoder(input);
                decoder >> transaction_id >> key_offset;
                return decoder.size() == 0;
        }

        static bool decode_remote_delete_request(
                MessagePiece piece, std::size_t key_size, const char *&key,
                uint64_t &target_row, uint64_t &request_sequence)
        {
                if (piece.get_message_type() != static_cast<uint32_t>(
                            TwoPLPashaMessage::REMOTE_DELETE_REQUEST) ||
                    piece.get_message_length() !=
                        TwoPLPashaMessageFactory::remote_delete_request_size(key_size))
                        return false;
                auto input = piece.toStringPiece();
                key = input.data();
                input.remove_prefix(key_size);
                Decoder decoder(input);
                decoder >> target_row >> request_sequence;
                return decoder.size() == 0;
        }

        static bool decode_scan_migration_request(
                MessagePiece piece, std::size_t key_size, const char *&min_key,
                const char *&max_key, uint64_t &limit, uint64_t &transaction_id,
                uint32_t &key_offset)
        {
                if (piece.get_message_type() != static_cast<uint32_t>(
                            TwoPLPashaMessage::DATA_MIGRATION_REQUEST_FOR_SCAN) ||
                    piece.get_message_length() !=
                        TwoPLPashaMessageFactory::scan_migration_request_size(key_size))
                        return false;
                auto input = piece.toStringPiece();
                min_key = input.data();
                input.remove_prefix(key_size);
                max_key = input.data();
                input.remove_prefix(key_size);
                Decoder decoder(input);
                decoder >> limit >> transaction_id >> key_offset;
                return decoder.size() == 0;
        }

        static bool decode_data_migration_response(
                MessagePiece piece, MigrationResponseOutcome &outcome,
                uint32_t &key_offset)
        {
                if (piece.get_message_type() != static_cast<uint32_t>(
                            TwoPLPashaMessage::DATA_MIGRATION_RESPONSE) ||
                    piece.get_message_length() !=
                        TwoPLPashaMessageFactory::status_key_offset_response_size())
                        return false;
                Decoder decoder(piece.toStringPiece());
                uint8_t raw = 0;
                decoder >> raw >> key_offset;
                if (decoder.size() != 0 || raw > 3) return false;
                switch (raw) {
                        case 0: outcome = MigrationResponseOutcome::Migrated; break;
                        case 1: outcome = MigrationResponseOutcome::Missing; break;
                        case 2: outcome = MigrationResponseOutcome::Busy; break;
                        case 3: outcome = MigrationResponseOutcome::NoMemory; break;
                }
                return true;
        }

        static bool decode_remote_insert_response(
                MessagePiece piece, RemoteInsertOutcome &outcome,
                uint32_t &key_offset)
        {
                if (piece.get_message_type() != static_cast<uint32_t>(
                            TwoPLPashaMessage::REMOTE_INSERT_RESPONSE) ||
                    piece.get_message_length() !=
                        TwoPLPashaMessageFactory::status_key_offset_response_size())
                        return false;
                Decoder decoder(piece.toStringPiece());
                uint8_t raw = 0;
                decoder >> raw >> key_offset;
                if (decoder.size() != 0 || raw > 3) return false;
                switch (raw) {
                        case 0: outcome = RemoteInsertOutcome::Inserted; break;
                        case 1: outcome = RemoteInsertOutcome::AlreadyExists; break;
                        case 2: outcome = RemoteInsertOutcome::Busy; break;
                        case 3: outcome = RemoteInsertOutcome::NoMemory; break;
                }
                return true;
        }

        static bool decode_scan_migration_response(
                MessagePiece piece, bool &success, uint32_t &key_offset)
        {
                if (piece.get_message_type() != static_cast<uint32_t>(
                            TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN) ||
                    piece.get_message_length() !=
                        TwoPLPashaMessageFactory::bool_key_offset_response_size())
                        return false;
                Decoder decoder(piece.toStringPiece());
                decoder >> success >> key_offset;
                return decoder.size() == 0;
        }

        static bool decode_remote_delete_response(
                MessagePiece piece, RemoteDeleteOutcome &outcome,
                uint32_t &key_offset, uint64_t &target_row,
                uint64_t &request_sequence)
        {
                if (piece.get_message_type() != static_cast<uint32_t>(
                            TwoPLPashaMessage::REMOTE_DELETE_RESPONSE) ||
                    piece.get_message_length() !=
                        TwoPLPashaMessageFactory::remote_delete_response_size())
                        return false;
                Decoder decoder(piece.toStringPiece());
                uint8_t raw = 0;
                decoder >> raw >> key_offset >> target_row >> request_sequence;
                if (decoder.size() != 0 || raw > 2) return false;
                switch (raw) {
                        case 0: outcome = RemoteDeleteOutcome::Deleted; break;
                        case 1: outcome = RemoteDeleteOutcome::Busy; break;
                        case 2: outcome = RemoteDeleteOutcome::Failed; break;
                }
                return true;
        }

        static void append_data_migration_response(
                Message &message, std::size_t table_id, std::size_t partition_id,
                MigrationResponseOutcome outcome, uint32_t key_offset)
        {
                const auto size =
                    TwoPLPashaMessageFactory::status_key_offset_response_size();
                Encoder encoder(message.data);
                encoder << MessagePiece::construct_message_piece_header(
                    static_cast<uint32_t>(TwoPLPashaMessage::DATA_MIGRATION_RESPONSE),
                    size, table_id, partition_id);
                switch (outcome) {
                        case MigrationResponseOutcome::Migrated:
                                encoder << uint8_t{0} << key_offset; message.flush(); return;
                        case MigrationResponseOutcome::Missing:
                                encoder << uint8_t{1} << key_offset; message.flush(); return;
                        case MigrationResponseOutcome::Busy:
                                encoder << uint8_t{2} << key_offset; message.flush(); return;
                        case MigrationResponseOutcome::NoMemory:
                                encoder << uint8_t{3} << key_offset; message.flush(); return;
                }
                LOG(FATAL) << "unknown migration response outcome";
        }

        static void append_remote_insert_response(
                Message &message, std::size_t table_id, std::size_t partition_id,
                RemoteInsertOutcome outcome, uint32_t key_offset)
        {
                const auto size =
                    TwoPLPashaMessageFactory::status_key_offset_response_size();
                Encoder encoder(message.data);
                encoder << MessagePiece::construct_message_piece_header(
                    static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_INSERT_RESPONSE),
                    size, table_id, partition_id);
                switch (outcome) {
                        case RemoteInsertOutcome::Inserted:
                                encoder << uint8_t{0} << key_offset; message.flush(); return;
                        case RemoteInsertOutcome::AlreadyExists:
                                encoder << uint8_t{1} << key_offset; message.flush(); return;
                        case RemoteInsertOutcome::Busy:
                                encoder << uint8_t{2} << key_offset; message.flush(); return;
                        case RemoteInsertOutcome::NoMemory:
                                encoder << uint8_t{3} << key_offset; message.flush(); return;
                }
                LOG(FATAL) << "unknown remote insert outcome";
        }

        static void append_scan_migration_response(
                Message &message, std::size_t table_id, std::size_t partition_id,
                bool success, uint32_t key_offset)
        {
                const auto size =
                    TwoPLPashaMessageFactory::bool_key_offset_response_size();
                Encoder encoder(message.data);
                encoder << MessagePiece::construct_message_piece_header(
                    static_cast<uint32_t>(
                        TwoPLPashaMessage::DATA_MIGRATION_RESPONSE_FOR_SCAN),
                    size, table_id, partition_id);
                encoder << success << key_offset;
                message.flush();
        }

        static void append_remote_delete_response(
                Message &message, std::size_t table_id, std::size_t partition_id,
                RemoteDeleteOutcome outcome, uint32_t key_offset,
                uint64_t target_row, uint64_t request_sequence)
        {
                const auto size =
                    TwoPLPashaMessageFactory::remote_delete_response_size();
                Encoder encoder(message.data);
                encoder << MessagePiece::construct_message_piece_header(
                    static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_DELETE_RESPONSE),
                    size, table_id, partition_id);
                switch (outcome) {
                        case RemoteDeleteOutcome::Deleted:
                                encoder << uint8_t{0} << key_offset
                                        << target_row << request_sequence;
                                message.flush(); return;
                        case RemoteDeleteOutcome::Busy:
                                encoder << uint8_t{1} << key_offset
                                        << target_row << request_sequence;
                                message.flush(); return;
                        case RemoteDeleteOutcome::Failed:
                                encoder << uint8_t{2} << key_offset
                                        << target_row << request_sequence;
                                message.flush(); return;
                }
                LOG(FATAL) << "unknown remote delete response outcome";
        }

        // The owner half of the original scan migration request.  Keeping it
        // here lets the transaction handler and the transaction-free KV
        // facade share the exact ITable scan / move_row_in sequence instead
        // of growing a second range-paging protocol in KVPartition.
        //
        // The owner half of the original scan migration request.  Keeping it
        // here lets the transaction handler and the transaction-free KV
        // facade share the exact ITable scan / move_row_in sequence instead
        // of growing a second range-paging protocol in KVPartition.
        // Master deliberately scans then move_row_in without one outer Clock
        // section (acknowledged race if a row is deleted between the two).
        static void move_in_scan_range(ITable &table, const void *min_key,
                                       const void *max_key, uint64_t limit)
        {
                // Extracted from master data_migration_request_for_scan_handler
                // (scan + per-key move_row_in). KV owns response / move_out order.
                std::vector<ITable::row_entity> scan_results;
                const auto append_scan_row =
                    [&](const void *key, ITable::MetaDataType *meta,
                        void *data) {
                        if (btreeolc_cxl::IsTreeDataAddress(key)) {
                                scan_results.emplace_back();
                                auto &row = scan_results.back();
                                row.key_size = table.key_size();
                                row.meta = meta;
                                row.data = data;
                                row.row_size = table.value_size();
                                latency_sim::FixedLatencyCopySharedToLocal(
                                    btreeolc_cxl::TreeDataDomain(), row.key,
                                    key, row.key_size);
                        } else {
                                scan_results.emplace_back(
                                    key, table.key_size(), meta, data,
                                    table.value_size());
                        }
                    };
                table.scan(min_key, [&](const void *key,
                                        ITable::MetaDataType *meta,
                                        void *data, bool) -> bool {
                        DCHECK(key != nullptr);
                        DCHECK(meta != nullptr);
                        DCHECK(data != nullptr);
                        bool migrating_next_key = false;
                        if (limit != 0 && scan_results.size() == limit) {
                                migrating_next_key = true;
                        } else if (table.compare_key(key, max_key) > 0) {
                                migrating_next_key = true;
                        }
                        if (table.compare_key(key, min_key) >= 0) {
                                if (scan_results.size() > 0) {
                                        if (table.compare_key(
                                                key,
                                                scan_results[scan_results.size() -
                                                             1]
                                                    .key) <= 0) {
                                                return false;
                                        }
                                }
                        } else {
                                return false;
                        }
                        append_scan_row(key, meta, data);
                        return migrating_next_key;
                });
                for (int i = 0; i < static_cast<int>(scan_results.size()); i++) {
                        std::tuple<ITable::MetaDataType *, void *> row_tuple(
                            scan_results[i].meta, scan_results[i].data);
                        migration_manager->move_row_in(
                            &table, scan_results[i].key, row_tuple, false);
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

                CXLTableBase *target_cxl_table = twopl_pasha_global_helper->get_cxl_table(table_id, partition_id);
                auto adjacency_ok = [&](const void *key, void *cxl_row, bool,
                                        size_t result_count, bool) -> bool {
                        auto *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cxl_row);
                        smeta->lock();
                        const bool ok = TwoPLPashaHelper::scan_row_adjacency_ok(
                            table.compare_key(key, min_key) == 0,
                            result_count == limit,
                            smeta->get_prev_key_real_bit(),
                            smeta->get_next_key_real_bit());
                        smeta->unlock();
                        return ok;
                };
                auto acquire_remote = [&](const void *key, void *cxl_row, bool,
                                          ITable::row_entity *row) -> bool {
                        bool lock_success = false;
                        if (type == TwoPLPashaRWKey::SCAN_FOR_READ) {
                                twopl_pasha_global_helper->remote_read_lock_and_inc_ref_cnt(reinterpret_cast<char *>(cxl_row), table.value_size(), lock_success);
                        } else if (type == TwoPLPashaRWKey::SCAN_FOR_UPDATE ||
                                   type == TwoPLPashaRWKey::SCAN_FOR_INSERT ||
                                   type == TwoPLPashaRWKey::SCAN_FOR_DELETE) {
                                twopl_pasha_global_helper->remote_write_lock_and_inc_ref_cnt(reinterpret_cast<char *>(cxl_row), table.value_size(), lock_success);
                        } else {
                                DCHECK(0);
                        }
                        if (!lock_success) return false;
                        auto *smeta = reinterpret_cast<TwoPLPashaMetadataShared *>(cxl_row);
                        *row = ITable::row_entity(key, table.key_size(),
                            reinterpret_cast<ITable::MetaDataType *>(cxl_row),
                            smeta->get_scc_data()->data, table.value_size());
                        return true;
                };
                ITable::row_entity next_row;
                bool has_next_row = false;
                bool scan_success = false;
                bool migration_required = false;
                bool scan_busy = false;
                TwoPLPashaHelper::scan_remote_fragment(
                    [&table](const void *left, const void *right) {
                        return table.compare_key(left, right);
                    }, *target_cxl_table, min_key, max_key, limit, scan_results,
                    &next_row, has_next_row, scan_success, migration_required,
                    scan_busy, adjacency_ok, acquire_remote,
                    [](const ITable::row_entity &row) -> const void * { return row.key; });
                if (has_next_row) {
                        scanKey.set_next_row_entity(next_row);
                        scanKey.set_next_row_locked();
                }

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
		DCHECK(inputPiece.get_message_type() == static_cast<uint32_t>(TwoPLPashaMessage::REMOTE_INSERT_RESPONSE));
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
		const char *key = nullptr;
		uint64_t target_row = 0;
		uint64_t request_sequence = 0;
		if (!decode_remote_delete_request(inputPiece, key_size, key, target_row,
		                                  request_sequence))
			return;
		(void)target_row;
		(void)request_sequence;

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
		v.push_back(static_cast<void (*)(MessagePiece, Message &, ITable &, Transaction *)>(data_migration_request_handler));
		v.push_back(data_migration_response_handler);
		v.push_back(static_cast<void (*)(MessagePiece, Message &, ITable &, Transaction *)>(data_migration_request_for_scan_handler));
		v.push_back(data_migration_response_for_scan_handler);
                v.push_back(data_move_out_hint_handler);
                v.push_back(static_cast<void (*)(MessagePiece, Message &, ITable &, Transaction *)>(remote_insert_request_handler));
                v.push_back(remote_insert_response_handler);
                v.push_back(static_cast<void (*)(MessagePiece, Message &, ITable &, Transaction *)>(remote_delete_request_handler));
                // replication is not supported
                // v.push_back(replication_request_handler);
		// v.push_back(replication_response_handler);
		return v;
	}
};

} // namespace star
