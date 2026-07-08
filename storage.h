#ifndef STORAGE_H
#define STORAGE_H

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <set>
#include <atomic>
#include <unordered_map>
#include "types.h"
#include "kvstore.h"
#include "index.h"
#include "catalog.h"
#include "wal.h"
#include "lock_manager.h"

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
    LockManager lockManager;
    
public:
    StorageManager(const std::string &dataDir = "./data");
    
    // Transaction management
    std::string beginTransaction(const std::string &sessionId = "");
    void commitTransaction(const std::string &sessionId = "");
    void rollbackTransaction(const std::string &sessionId = "");
    bool isInTransaction(const std::string &sessionId = "") const;
    bool hasActiveTransaction(const std::string &sessionId = "") const;
    void rollbackAllTransactions();
    
    // Locking and isolation
    void acquireReadLock(const std::string &tableName, const std::string &rowKey = "", const std::string &sessionId = "");
    void acquireWriteLock(const std::string &tableName, const std::string &rowKey = "", const std::string &sessionId = "");

    // Data operations
    void insertRecord(int tableId, const std::string &tableName, const Record &record, const std::string &sessionId = "");
    void deleteRecord(int tableId, const Record &record, const std::string &sessionId = "");
    void updateRecord(int tableId, const Record &oldRecord, const Record &newRecord, const std::string &sessionId = "");
    
    // Schema operations
    void createTable(const std::string &tableName, const std::vector<ColumnMetadata> &columns, const std::string &sessionId = "");
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
    struct TransactionContext
    {
        bool inTransaction = false;
        std::vector<WalEntry> transactionLog;
        std::string transactionSnapshot;
        int currentTransactionId = 0;
    };

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
    std::unordered_map<std::string, TransactionContext> transactionContexts;
    std::atomic<int> nextTransactionId{1};
    std::atomic<int> nextSessionId{1};

    std::string normalizeSessionId(const std::string &sessionId) const;
    StorageManager::TransactionContext &getOrCreateTransactionContext(const std::string &sessionId);
    const StorageManager::TransactionContext *getTransactionContext(const std::string &sessionId) const;
    void applyWalEntry(const WalEntry &entry);
    void clearDatabaseState();
    void deduplicatePrimaryKeys();
    void rebuildIndexesFromData();
    void releaseTransactionLocksLocked(int transactionId);
    void acquireLockLocked(const std::string &resourceId, bool exclusive, const std::string &sessionId);
    std::string makeLockResource(const std::string &tableName, const std::string &rowKey = "") const;
    void indexRecord(const TableMetadata &tm, const Record &record);
    void unindexRecord(const TableMetadata &tm, const Record &record);
    std::string serializeDatabase();
    void deserializeDatabase(const std::string &data);
};

#endif // STORAGE_H
