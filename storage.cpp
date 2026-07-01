#include "storage.h"
#include "encode.h"
#include "table.h"
#include <iostream>
#include <sstream>
#include <ctime>
#include <cstdint>
#include <cctype>
#include <set>

namespace
{
    std::string lowerCopy(const std::string &value)
    {
        std::string result = value;
        for (char &ch : result)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return result;
    }

    int primaryKeyIndex(const TableMetadata &tm)
    {
        return (tm.primaryKeyIndex >= 0) ? tm.primaryKeyIndex : 0;
    }

    int findColumnIndex(const TableMetadata &tm, const std::string &columnName)
    {
        std::string normalized = lowerCopy(columnName);
        for (size_t i = 0; i < tm.columns.size(); i++)
        {
            if (tm.columns[i].name == normalized)
                return static_cast<int>(i);
        }
        return -1;
    }

    int nameColumnIndex(const TableMetadata &tm)
    {
        return findColumnIndex(tm, "name");
    }

    bool primaryKeyValue(const TableMetadata &tm, const Record &record, Value &value)
    {
        int pk = primaryKeyIndex(tm);
        if (pk < 0 || pk >= static_cast<int>(record.vals.size()))
            return false;
        value = record.vals[pk];
        return true;
    }

    std::vector<char> recordKey(int tableId, const TableMetadata &tm, const Record &record)
    {
        Value pk;
        if (!primaryKeyValue(tm, record, pk))
            return {};
        return encodeKey(tableId, {pk});
    }
}

StorageManager::StorageManager(const std::string &dataDir)
    : dataDir(dataDir)
{
    wal = std::make_unique<WriteAheadLog>(dataDir);
    load();
}

bool StorageManager::isInTransaction() const
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    return inTransaction;
}

void StorageManager::beginTransaction()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    if (inTransaction)
        throw std::runtime_error("Transaction already in progress");

    inTransaction = true;
    transactionLog.clear();
    transactionSnapshot = serializeDatabase();
}

void StorageManager::commitTransaction()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    if (!inTransaction)
        throw std::runtime_error("No transaction in progress");

    // Write all buffered entries to WAL before applying them to the in-memory store.
    for (const auto &entry : transactionLog)
        wal->logOperation(entry);

    // Now that WAL entries have been durably appended, apply them to the in-memory
    // KV store so the committed state becomes visible.
    for (const auto &entry : transactionLog)
        applyWalEntry(entry);

    // Clear transaction state and persist a checkpoint.
    inTransaction = false;
    transactionSnapshot.clear();
    // Checkpoint will clear the WAL once the snapshot is safely written.
    save();
    transactionLog.clear();
}

void StorageManager::rollbackTransaction()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    if (!inTransaction)
        throw std::runtime_error("No transaction in progress");

    // Restore the pre-transaction snapshot.
    clearDatabaseState();
    deserializeDatabase(transactionSnapshot);
    rebuildIndexesFromData();

    inTransaction = false;
    transactionLog.clear();
    transactionSnapshot.clear();
    // No save() here — we intentionally don't persist rolled-back data.
}

void StorageManager::insertRecord(int tableId, const std::string &tableName, const Record &record)
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    std::string normalizedTableName = lowerCopy(tableName);

    if (!catalog.tableExists(normalizedTableName))
        throw std::runtime_error("Table not found: " + tableName);

    TableMetadata tm = catalog.getTable(normalizedTableName);
    int effectiveTableId = tableId;
    if (effectiveTableId == 0)
        effectiveTableId = getTableId(normalizedTableName);

    if (effectiveTableId == 0)
        throw std::runtime_error("Table not found: " + tableName);

    int pkIndex = tm.primaryKeyIndex >= 0 ? tm.primaryKeyIndex : 0;
    if (pkIndex < 0 || pkIndex >= static_cast<int>(record.vals.size()))
        throw std::runtime_error("Invalid primary key value");

    std::vector<char> key = encodeKey(effectiveTableId, {record.vals[pkIndex]});

    // Duplicate PK check in the committed store.
    if (!kvStore.get(key).empty())
        throw std::runtime_error("Duplicate primary key value");

    // Duplicate PK check within an in-progress transaction buffer.
    for (const auto &pending : transactionLog)
    {
        if (pending.operation != WalOperationType::Insert)
            continue;
        if (pending.tableId != effectiveTableId)
            continue;
        if (pending.newRecord.vals.size() <= static_cast<size_t>(pkIndex))
            continue;
        if (encodeKey(effectiveTableId, {pending.newRecord.vals[pkIndex]}) == key)
            throw std::runtime_error("Duplicate primary key value");
    }

    WalEntry entry;
    entry.operation  = WalOperationType::Insert;
    entry.timestamp  = std::time(nullptr);
    entry.tableId    = effectiveTableId;
    entry.tableName  = normalizedTableName;
    entry.key        = key;
    entry.value      = serialize(record);
    entry.newRecord  = record;

    if (inTransaction)
    {
        transactionLog.push_back(entry);
    }
    else
    {
        wal->logOperation(entry);
        applyWalEntry(entry);
        save();
    }
}

void StorageManager::deleteRecord(int tableId, const Record &record)
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);

    WalEntry entry;
    entry.operation = WalOperationType::Delete;
    entry.timestamp = std::time(nullptr);
    entry.tableId   = tableId;

    auto it = tableIdToName.find(tableId);
    if (it != tableIdToName.end())
        entry.tableName = lowerCopy(it->second);

    if (!entry.tableName.empty() && catalog.tableExists(entry.tableName))
    {
        TableMetadata tm = catalog.getTable(entry.tableName);
        entry.key   = recordKey(tableId, tm, record);
        entry.value = serialize(record);
    }
    entry.oldRecord = record;

    if (inTransaction)
    {
        transactionLog.push_back(entry);
    }
    else
    {
        wal->logOperation(entry);
        applyWalEntry(entry);
        save();
    }
}

void StorageManager::updateRecord(int tableId, const Record &oldRecord, const Record &newRecord)
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);

    WalEntry entry;
    entry.operation = WalOperationType::Update;
    entry.timestamp = std::time(nullptr);
    entry.tableId   = tableId;

    auto it = tableIdToName.find(tableId);
    if (it != tableIdToName.end())
        entry.tableName = lowerCopy(it->second);

    if (!entry.tableName.empty() && catalog.tableExists(entry.tableName))
    {
        TableMetadata tm = catalog.getTable(entry.tableName);
        entry.key   = recordKey(tableId, tm, oldRecord);
        entry.value = serialize(newRecord);
    }
    entry.oldRecord = oldRecord;
    entry.newRecord = newRecord;

    if (inTransaction)
    {
        transactionLog.push_back(entry);
    }
    else
    {
        wal->logOperation(entry);
        applyWalEntry(entry);
        save();
    }
}

void StorageManager::createTable(const std::string &tableName, const std::vector<ColumnMetadata> &columns)
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    std::string normalizedTableName = lowerCopy(tableName);

    if (catalog.tableExists(normalizedTableName))
        throw std::runtime_error("Table already exists: " + tableName);

    WalEntry entry;
    entry.operation = WalOperationType::CreateTable;
    entry.timestamp = std::time(nullptr);
    entry.tableId   = nextTableId;
    entry.tableName = normalizedTableName;
    entry.columns   = columns;

    if (inTransaction)
    {
        transactionLog.push_back(entry);
    }
    else
    {
        wal->logOperation(entry);
        applyWalEntry(entry);
        save();
    }
}

bool StorageManager::tableExists(const std::string &tableName) const
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    return catalog.tableExists(tableName);
}

int StorageManager::getTableId(const std::string &tableName) const
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    auto it = tableNameToId.find(lowerCopy(tableName));
    if (it != tableNameToId.end())
        return it->second;
    return 0;
}

void StorageManager::applyWalEntry(const WalEntry &entry)
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    try
    {
        int tid = entry.tableId;
        std::string tableName = lowerCopy(entry.tableName);

        // Resolve tableId from name if the id is missing or unknown.
        if ((tid == 0 || tableIdToName.find(tid) == tableIdToName.end()) && !tableName.empty())
        {
            auto byName = tableNameToId.find(tableName);
            if (byName != tableNameToId.end())
                tid = byName->second;
        }

        if (entry.operation == WalOperationType::CreateTable)
        {
            if (tableName.empty())
                return;

            if (!catalog.tableExists(tableName))
                catalog.createTable(tableName, entry.columns);

            if (tid == 0)
                tid = nextTableId;

            tableIdToName[tid]    = tableName;
            tableNameToId[tableName] = tid;
            if (tid >= nextTableId)
                nextTableId = tid + 1;
            return;
        }

        // For non-CreateTable operations, resolve the table name from id if needed.
        if (tableName.empty())
        {
            auto it = tableIdToName.find(tid);
            if (it != tableIdToName.end())
                tableName = it->second;
        }

        if (tableName.empty() || !catalog.tableExists(tableName) || tid == 0)
            return;

        TableMetadata tm = catalog.getTable(tableName);

        if (entry.operation == WalOperationType::Insert)
        {
            if (entry.newRecord.vals.empty())
                return;

            std::vector<char> key = recordKey(tid, tm, entry.newRecord);
            if (key.empty())
                return;

            kvStore.put(key, serialize(entry.newRecord));
            indexRecord(tm, entry.newRecord);
        }
        else if (entry.operation == WalOperationType::Delete)
        {
            if (entry.oldRecord.vals.empty())
                return;

            std::vector<char> key = recordKey(tid, tm, entry.oldRecord);
            if (key.empty())
                return;

            unindexRecord(tm, entry.oldRecord);
            kvStore.del(key);
        }
        else if (entry.operation == WalOperationType::Update)
        {
            if (entry.oldRecord.vals.empty() || entry.newRecord.vals.empty())
                return;

            std::vector<char> oldKey = recordKey(tid, tm, entry.oldRecord);
            std::vector<char> newKey = recordKey(tid, tm, entry.newRecord);
            if (oldKey.empty() || newKey.empty())
                return;

            unindexRecord(tm, entry.oldRecord);
            if (oldKey != newKey)
                kvStore.del(oldKey);

            kvStore.put(newKey, serialize(entry.newRecord));
            indexRecord(tm, entry.newRecord);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error applying WAL entry: " << e.what() << std::endl;
    }
}

void StorageManager::clearDatabaseState()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    kvStore.clear();
    index.clear();
    catalog       = Catalog();
    tableIdToName.clear();
    tableNameToId.clear();
    nextTableId   = 1;
}

void StorageManager::indexRecord(const TableMetadata &tm, const Record &record)
{
    int indexedColumn = nameColumnIndex(tm);
    if (indexedColumn < 0 || indexedColumn >= static_cast<int>(record.vals.size()))
        return;

    Value pk;
    if (!primaryKeyValue(tm, record, pk))
        return;

    const Value &indexedValue = record.vals[indexedColumn];
    if (indexedValue.type == TYPE_STRING && pk.type == TYPE_INT)
        index.insert(indexedValue.s, pk.i);
}

void StorageManager::unindexRecord(const TableMetadata &tm, const Record &record)
{
    int indexedColumn = nameColumnIndex(tm);
    if (indexedColumn < 0 || indexedColumn >= static_cast<int>(record.vals.size()))
        return;

    Value pk;
    if (!primaryKeyValue(tm, record, pk))
        return;

    const Value &indexedValue = record.vals[indexedColumn];
    if (indexedValue.type == TYPE_STRING && pk.type == TYPE_INT)
        index.remove(indexedValue.s, pk.i);
}

void StorageManager::save()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    try
    {
        std::string snapshot = serializeDatabase();
        wal->checkpoint(snapshot);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error saving database: " << e.what() << std::endl;
    }
}

void StorageManager::load()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    try
    {
        std::string checkpoint = wal->readCheckpoint();
        if (!checkpoint.empty())
            deserializeDatabase(checkpoint);

        deduplicatePrimaryKeys();
        rebuildIndexesFromData();
        recover();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error loading database: " << e.what() << std::endl;
    }
}

void StorageManager::recover()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    try
    {
        if (!wal->hasWalEntries())
            return;

        std::cout << "Recovering from WAL..." << std::endl;
        auto entries = wal->readWalEntries();
        for (const auto &entry : entries)
            applyWalEntry(entry);

        std::cout << "Recovery complete. Applied " << entries.size() << " entries." << std::endl;

        // Checkpoint the recovered state and clear the WAL.
        save();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error during recovery: " << e.what() << std::endl;
    }
}

void StorageManager::rebuildIndexesFromData()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    index.clear();

    for (const auto &entry : tableIdToName)
    {
        int tableId = entry.first;
        const std::string &tableName = entry.second;

        if (!catalog.tableExists(tableName))
            continue;

        TableMetadata tm = catalog.getTable(tableName);
        Table table;
        table.name            = tableName;
        table.tableId         = tableId;
        table.primaryKeyIndex = (tm.primaryKeyIndex >= 0) ? tm.primaryKeyIndex : 0;
        for (const auto &column : tm.columns)
        {
            table.cols.push_back(column.name);
            table.types.push_back(column.type == "INT" ? TYPE_INT : TYPE_STRING);
        }

        std::vector<Record> records = fullTableScan(kvStore, table);
        for (const auto &record : records)
            indexRecord(tm, record);
    }
}

void StorageManager::deduplicatePrimaryKeys()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);

    for (const auto &entry : tableIdToName)
    {
        int tableId = entry.first;
        std::vector<char> startKey = encodePrefix(tableId);
        std::vector<char> endKey   = encodePrefix(tableId + 1);
        auto entries = kvStore.scanRange(startKey, endKey);
        std::set<std::vector<char>> seenKeys;

        for (const auto &kv : entries)
        {
            if (!seenKeys.insert(kv.first).second)
                kvStore.del(kv.first);
        }
    }
}

std::string StorageManager::serializeDatabase()
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    std::string out;

    auto write_u32 = [&](uint32_t v)
    {
        for (int i = 3; i >= 0; --i)
            out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    };

    auto write_bytes = [&](const std::string &s)
    {
        out.append(s.data(), s.size());
    };

    // Table metadata section.
    write_u32(static_cast<uint32_t>(tableIdToName.size()));

    for (const auto &p : tableIdToName)
    {
        write_u32(static_cast<uint32_t>(p.first));
        write_u32(static_cast<uint32_t>(p.second.size()));
        write_bytes(p.second);

        if (catalog.tableExists(p.second))
        {
            TableMetadata tm = catalog.getTable(p.second);
            write_u32(static_cast<uint32_t>(tm.columns.size()));
            for (const auto &c : tm.columns)
            {
                write_u32(static_cast<uint32_t>(c.name.size()));
                write_bytes(c.name);
                write_u32(static_cast<uint32_t>(c.type.size()));
                write_bytes(c.type);
                uint32_t flags = (c.isPrimaryKey ? 1u : 0u) | (c.isNotNull ? 2u : 0u);
                write_u32(flags);
            }
        }
        else
        {
            write_u32(0);
        }
    }

    // KV data section — one scanRange per table.
    std::vector<std::pair<std::vector<char>, std::vector<char>>> allEntries;
    for (const auto &p : tableIdToName)
    {
        std::vector<char> startKey = encodePrefix(p.first);
        std::vector<char> endKey   = encodePrefix(p.first + 1);
        auto entries = kvStore.scanRange(startKey, endKey);
        for (auto &e : entries)
            allEntries.push_back(e);
    }

    write_u32(static_cast<uint32_t>(allEntries.size()));
    for (auto &e : allEntries)
    {
        write_u32(static_cast<uint32_t>(e.first.size()));
        out.append(e.first.data(), e.first.size());
        write_u32(static_cast<uint32_t>(e.second.size()));
        out.append(e.second.data(), e.second.size());
    }

    return out;
}

void StorageManager::deserializeDatabase(const std::string &data)
{
    std::lock_guard<std::recursive_mutex> lock(storageMutex);
    try
    {
        size_t offset = 0;

        auto read_u32 = [&](uint32_t &out) -> bool
        {
            if (offset + 4 > data.size()) return false;
            out = 0;
            for (int i = 0; i < 4; ++i)
                out = (out << 8) | static_cast<uint8_t>(data[offset + i]);
            offset += 4;
            return true;
        };

        auto read_str = [&](uint32_t len, std::string &s) -> bool
        {
            if (offset + len > data.size()) return false;
            s.assign(&data[offset], len);
            offset += len;
            return true;
        };

        uint32_t numTables = 0;
        if (!read_u32(numTables)) return;

        for (uint32_t i = 0; i < numTables; ++i)
        {
            uint32_t id = 0;
            if (!read_u32(id)) return;

            uint32_t nameLen = 0;
            if (!read_u32(nameLen)) return;

            std::string name;
            if (!read_str(nameLen, name)) return;

            uint32_t colCount = 0;
            if (!read_u32(colCount)) return;

            std::vector<ColumnMetadata> cols;
            for (uint32_t c = 0; c < colCount; ++c)
            {
                uint32_t cn = 0; read_u32(cn);
                std::string cname; read_str(cn, cname);
                uint32_t ct = 0; read_u32(ct);
                std::string ctype; read_str(ct, ctype);
                uint32_t flags = 0; read_u32(flags);

                ColumnMetadata cm;
                cm.name        = cname;
                cm.type        = ctype;
                cm.isPrimaryKey = (flags & 1u) != 0;
                cm.isNotNull    = (flags & 2u) != 0;
                cols.push_back(cm);
            }

            if (!catalog.tableExists(name))
                catalog.createTable(name, cols);

            std::string normalizedName = lowerCopy(name);
            tableIdToName[static_cast<int>(id)] = normalizedName;
            tableNameToId[normalizedName]        = static_cast<int>(id);
            if (static_cast<int>(id) >= nextTableId)
                nextTableId = static_cast<int>(id) + 1;
        }

        uint32_t numEntries = 0;
        if (!read_u32(numEntries)) return;

        for (uint32_t i = 0; i < numEntries; ++i)
        {
            uint32_t ks = 0; read_u32(ks);
            std::string k; read_str(ks, k);
            uint32_t vs = 0; read_u32(vs);
            std::string v; read_str(vs, v);

            std::vector<char> key(k.begin(), k.end());
            std::vector<char> val(v.begin(), v.end());
            kvStore.put(key, val);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error deserializing database: " << e.what() << std::endl;
    }
}