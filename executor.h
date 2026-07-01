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
    bool indexUsed = false;
    std::vector<std::string> columns;
    std::vector<Record> rows;
    int rowsAffected = 0;
};

class Executor
{
private:
    std::unique_ptr<StorageManager> storageManager;
    bool inTransaction = false;

public:
    explicit Executor(const std::string &dataDir = "./coredb_data");

    QueryResult execute(const SqlStatement &statement);

    const Catalog &getCatalog() const;
    std::vector<std::string> listTables() const;
    void shutdown();

private:
    QueryResult executeCreateTable(const SqlStatement &statement);
    QueryResult executeInsert(const SqlStatement &statement);
    QueryResult executeSelect(const SqlStatement &statement);
    QueryResult executeUpdate(const SqlStatement &statement);
    QueryResult executeDelete(const SqlStatement &statement);
    QueryResult executeBegin(const SqlStatement &statement);
    QueryResult executeCommit(const SqlStatement &statement);
    QueryResult executeRollback(const SqlStatement &statement);

    Value sqlLiteralToValue(const SqlLiteral &literal);
    Record buildRecordFromValues(const TableMetadata &tm, const std::vector<Value> &values);
};

#endif // EXECUTOR_H
