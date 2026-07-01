#ifndef STORAGE_H
#define STORAGE_H

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <set>
#include <unordered_map>
#include "types.h"
#include "kvstore.h"
#include "index.h"
#include "catalog.h"
#include "wal.h"

class StorageManager
{
private:
    std::string dataDir;
    std::unique_ptr<WriteAheadLog> wal;
    KVStore kvStore;
    Index index;
    Catalog catalog;
    std::map<int, std::string> tableIdToName;
    std::map<std::string, int> tableNameToId;
    int nextTableId = 1;
    mutable std::recursive_mutex storageMutex;
    
public:
    StorageManager(const std::string &dataDir = "./data");
    
    // Transaction management
    void beginTransaction();
    void commitTransaction();
    void rollbackTransaction();
    bool isInTransaction() const;
    
    // Locking and isolation
    void acquireReadLock(const std::string &tableName, const std::string &rowKey = "");
    void acquireWriteLock(const std::string &tableName, const std::string &rowKey = "");

    // Data operations
    void insertRecord(int tableId, const std::string &tableName, const Record &record);
    void deleteRecord(int tableId, const Record &record);
    void updateRecord(int tableId, const Record &oldRecord, const Record &newRecord);
    
    // Schema operations
    void createTable(const std::string &tableName, const std::vector<ColumnMetadata> &columns);
    bool tableExists(const std::string &tableName) const;
    int getTableId(const std::string &tableName) const;
    
    // Persistence
    void save();
    void load();
    void recover();
    
    // Getters
    KVStore& getKVStore() { return kvStore; }
    Index& getIndex() { return index; }
    Catalog& getCatalog() { return catalog; }
    const Catalog& getCatalog() const { return catalog; }
    
private:
    bool inTransaction = false;
    std::vector<WalEntry> transactionLog;
    std::string transactionSnapshot;
    int nextTransactionId = 1;
    int currentTransactionId = 0;

    struct LockInfo
    {
        int transactionId;
        bool exclusive;
    };

    struct TransactionLockState
    {
        std::set<std::string> resources;
    };

    std::mutex lockManagerMutex;
    std::condition_variable lockManagerCv;
    std::unordered_map<std::string, std::vector<LockInfo>> grantedLocks;
    std::unordered_map<int, TransactionLockState> transactionLocks;

    void applyWalEntry(const WalEntry &entry);
    void clearDatabaseState();
    void deduplicatePrimaryKeys();
    void rebuildIndexesFromData();
    void releaseTransactionLocksLocked(int transactionId);
    void acquireLockLocked(const std::string &resourceId, bool exclusive);
    std::string makeLockResource(const std::string &tableName, const std::string &rowKey = "") const;
    void indexRecord(const TableMetadata &tm, const Record &record);
    void unindexRecord(const TableMetadata &tm, const Record &record);
    std::string serializeDatabase();
    void deserializeDatabase(const std::string &data);
};

#endif // STORAGE_H
