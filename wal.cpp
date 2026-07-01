#include "wal.h"
#include "record.h"
#include <iostream>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    constexpr size_t MAX_WAL_BLOB_SIZE = 128 * 1024 * 1024;

    void writeBlob(std::ofstream &file, const std::vector<char> &data)
    {
        size_t size = data.size();
        file.write(reinterpret_cast<const char *>(&size), sizeof(size));
        if (size > 0)
            file.write(data.data(), size);
    }

    void writeString(std::ofstream &file, const std::string &value)
    {
        size_t size = value.size();
        file.write(reinterpret_cast<const char *>(&size), sizeof(size));
        if (size > 0)
            file.write(value.data(), size);
    }

    bool readBlob(std::ifstream &file, std::vector<char> &data)
    {
        size_t size = 0;
        if (!file.read(reinterpret_cast<char *>(&size), sizeof(size)))
            return false;

        if (size > MAX_WAL_BLOB_SIZE)
            return false;

        data.assign(size, '\0');
        if (size > 0 && !file.read(data.data(), size))
            return false;

        return true;
    }

    bool readString(std::ifstream &file, std::string &value)
    {
        std::vector<char> data;
        if (!readBlob(file, data))
            return false;

        value.assign(data.begin(), data.end());
        return true;
    }

    void writeRecord(std::ofstream &file, const Record &record)
    {
        writeBlob(file, serialize(record));
    }

    bool readRecord(std::ifstream &file, Record &record)
    {
        std::vector<char> data;
        if (!readBlob(file, data))
            return false;

        record = deserialize(data);
        return true;
    }

    void writeColumns(std::ofstream &file, const std::vector<ColumnMetadata> &columns)
    {
        size_t count = columns.size();
        file.write(reinterpret_cast<const char *>(&count), sizeof(count));

        for (const auto &column : columns)
        {
            writeString(file, column.name);
            writeString(file, column.type);
            char flags = 0;
            if (column.isPrimaryKey)
                flags |= 1;
            if (column.isNotNull)
                flags |= 2;
            file.write(&flags, sizeof(flags));
        }
    }

    bool readColumns(std::ifstream &file, std::vector<ColumnMetadata> &columns)
    {
        size_t count = 0;
        if (!file.read(reinterpret_cast<char *>(&count), sizeof(count)))
            return false;

        if (count > 4096)
            return false;

        columns.clear();
        columns.reserve(count);

        for (size_t i = 0; i < count; i++)
        {
            ColumnMetadata column;
            if (!readString(file, column.name))
                return false;
            if (!readString(file, column.type))
                return false;

            char flags = 0;
            if (!file.read(&flags, sizeof(flags)))
                return false;

            column.isPrimaryKey = (flags & 1) != 0;
            column.isNotNull = (flags & 2) != 0;
            columns.push_back(column);
        }

        return true;
    }
}

WriteAheadLog::WriteAheadLog(const std::string &dataDir)
{
    ensureDataDirectory(dataDir);
    walFilePath = dataDir + "/" + WAL_FILENAME;
    checkpointFilePath = dataDir + "/" + CHECKPOINT_FILENAME;
}

void WriteAheadLog::ensureDataDirectory(const std::string &dataDir)
{
    try
    {
        if (!fs::exists(dataDir))
        {
            fs::create_directories(dataDir);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error creating data directory: " << e.what() << std::endl;
    }
}

void WriteAheadLog::logOperation(const WalEntry &entry)
{
    try
    {
        std::ofstream walFile(walFilePath, std::ios::app | std::ios::binary);
        if (!walFile.is_open())
            throw std::runtime_error("Cannot open WAL file for writing");

        // Write entry header
        int op = static_cast<int>(entry.operation);
        walFile.write(reinterpret_cast<const char *>(&op), sizeof(op));
        walFile.write(reinterpret_cast<const char *>(&entry.timestamp), sizeof(entry.timestamp));
        walFile.write(reinterpret_cast<const char *>(&entry.tableId), sizeof(entry.tableId));

        writeString(walFile, entry.tableName);
        writeBlob(walFile, entry.key);
        writeBlob(walFile, entry.value);
        writeRecord(walFile, entry.oldRecord);
        writeRecord(walFile, entry.newRecord);
        writeColumns(walFile, entry.columns);

        walFile.close();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error writing WAL entry: " << e.what() << std::endl;
    }
}

std::vector<WalEntry> WriteAheadLog::readWalEntries()
{
    std::vector<WalEntry> entries;

    if (!fs::exists(walFilePath))
        return entries;

    try
    {
        std::ifstream walFile(walFilePath, std::ios::binary);
        if (!walFile.is_open())
            return entries;

        while (walFile.good())
        {
            WalEntry entry;

            // Read operation type
            int op;
            if (!walFile.read(reinterpret_cast<char *>(&op), sizeof(op)))
                break;
            entry.operation = static_cast<WalOperationType>(op);

            // Read timestamp and table ID
            if (!walFile.read(reinterpret_cast<char *>(&entry.timestamp), sizeof(entry.timestamp)))
                break;
            if (!walFile.read(reinterpret_cast<char *>(&entry.tableId), sizeof(entry.tableId)))
                break;

            if (!readString(walFile, entry.tableName))
                break;
            if (!readBlob(walFile, entry.key))
                break;
            if (!readBlob(walFile, entry.value))
                break;
            if (!readRecord(walFile, entry.oldRecord))
                break;
            if (!readRecord(walFile, entry.newRecord))
                break;
            if (!readColumns(walFile, entry.columns))
                break;

            entries.push_back(entry);
        }

        walFile.close();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error reading WAL entries: " << e.what() << std::endl;
    }

    return entries;
}

void WriteAheadLog::clearWal()
{
    try
    {
        if (fs::exists(walFilePath))
            fs::remove(walFilePath);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error clearing WAL: " << e.what() << std::endl;
    }
}

void WriteAheadLog::checkpoint(const std::string &dbSnapshot)
{
    try
    {
        // Write to a temp file first, then atomically rename so a crash
        // mid-write never leaves a corrupt checkpoint on disk.
        std::string tmpPath = checkpointFilePath + ".tmp";

        std::ofstream tmpFile(tmpPath, std::ios::binary | std::ios::trunc);
        if (!tmpFile.is_open())
            throw std::runtime_error("Cannot open checkpoint tmp file for writing");

        size_t snapshotSize = dbSnapshot.size();
        tmpFile.write(reinterpret_cast<const char *>(&snapshotSize), sizeof(snapshotSize));
        tmpFile.write(dbSnapshot.c_str(), snapshotSize);
        tmpFile.close();

        // Atomic replace: old checkpoint is only replaced once the new one is fully written.
        fs::rename(tmpPath, checkpointFilePath);

        // Only clear the WAL after the checkpoint is safely on disk.
        clearWal();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error creating checkpoint: " << e.what() << std::endl;
    }
}

std::string WriteAheadLog::readCheckpoint()
{
    // If a .tmp file exists the previous checkpoint write was interrupted by a crash.
    // Remove it — the WAL still has the entries and recover() will replay them.
    std::string tmpPath = checkpointFilePath + ".tmp";
    if (fs::exists(tmpPath))
    {
        std::cerr << "Warning: incomplete checkpoint file found, removing." << std::endl;
        try { fs::remove(tmpPath); } catch (...) {}
    }

    if (!fs::exists(checkpointFilePath))
        return "";

    try
    {
        std::ifstream checkpointFile(checkpointFilePath, std::ios::binary);
        if (!checkpointFile.is_open())
            return "";

        size_t snapshotSize;
        checkpointFile.read(reinterpret_cast<char *>(&snapshotSize), sizeof(snapshotSize));

        std::string snapshot(snapshotSize, '\0');
        checkpointFile.read(&snapshot[0], snapshotSize);
        checkpointFile.close();

        return snapshot;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error reading checkpoint: " << e.what() << std::endl;
    }

    return "";
}

bool WriteAheadLog::hasWalEntries() const
{
    return fs::exists(walFilePath) && fs::file_size(walFilePath) > 0;
}