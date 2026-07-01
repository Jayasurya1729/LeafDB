#ifndef WAL_H
#define WAL_H

#include <string>
#include <vector>
#include <fstream>
#include <ctime>
#include "types.h"
#include "catalog.h"

enum class WalOperationType
{
    Insert,
    Update,
    Delete,
    CreateTable
};

struct WalEntry
{
    WalOperationType operation;
    long timestamp;
    int tableId;
    std::string tableName;
    std::vector<char> key;
    std::vector<char> value;
    std::vector<ColumnMetadata> columns; // For CREATE TABLE recovery
    Record oldRecord;  // For updates
    Record newRecord;  // For inserts/updates
};

class WriteAheadLog
{
private:
    std::string walFilePath;
    std::string checkpointFilePath;
    const std::string WAL_FILENAME = "coredb.wal";
    const std::string CHECKPOINT_FILENAME = "coredb.checkpoint";

public:
    WriteAheadLog(const std::string &dataDir = "./data");
    
    // Write operation to WAL before applying
    void logOperation(const WalEntry &entry);
    
    // Read all entries from WAL
    std::vector<WalEntry> readWalEntries();
    
    // Clear WAL after checkpoint
    void clearWal();
    
    // Create checkpoint
    void checkpoint(const std::string &dbSnapshot);
    
    // Read checkpoint
    std::string readCheckpoint();
    
    // Check if data directory exists, create if not
    void ensureDataDirectory(const std::string &dataDir);
    
    bool hasWalEntries() const;
};

#endif // WAL_H
