#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "sql.h"
#include "catalog.h"
#include "table.h"
#include "storage.h"
#include <vector>
#include <string>
#include <memory>

struct QueryResult
{
    bool success = false;
    std::string message;
    std::string sessionId;
    bool indexUsed = false;
    std::vector<std::string> columns;
    std::vector<Record> rows;
    int rowsAffected = 0;
};

class Executor
{
private:
    std::unique_ptr<StorageManager> storageManager;
    inline static thread_local bool inTransaction = false;

public:
    explicit Executor(const std::string &dataDir = "./coredb_data");

    QueryResult execute(const SqlStatement &statement, const std::string &sessionId = "");

    const Catalog &getCatalog() const;
    std::vector<std::string> listTables() const;
    void shutdown();

private:
    QueryResult executeCreateTable(const SqlStatement &statement, const std::string &sessionId = "");
    QueryResult executeInsert(const SqlStatement &statement, const std::string &sessionId = "");
    QueryResult executeSelect(const SqlStatement &statement, const std::string &sessionId = "");
    QueryResult executeUpdate(const SqlStatement &statement, const std::string &sessionId = "");
    QueryResult executeDelete(const SqlStatement &statement, const std::string &sessionId = "");
    QueryResult executeBegin(const SqlStatement &statement, const std::string &sessionId = "");
    QueryResult executeCommit(const SqlStatement &statement, const std::string &sessionId = "");
    QueryResult executeRollback(const SqlStatement &statement, const std::string &sessionId = "");

    Value sqlLiteralToValue(const SqlLiteral &literal);
    Record buildRecordFromValues(const TableMetadata &tm, const std::vector<Value> &values);
};

#endif // EXECUTOR_H
