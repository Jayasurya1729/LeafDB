#include "executor.h"
#include "encode.h"
#include <sstream>
#include <set>
#include <cctype>
#include <stdexcept>

namespace
{
    Table makeTableFromMetadata(const TableMetadata &tm, int tableId)
    {
        Table t;
        t.name            = tm.name;
        t.tableId         = tableId;
        t.primaryKeyIndex = (tm.primaryKeyIndex >= 0) ? tm.primaryKeyIndex : 0;
        for (const auto &column : tm.columns)
        {
            t.cols.push_back(column.name);
            t.types.push_back(column.type == "INT" ? TYPE_INT : TYPE_STRING);
        }
        return t;
    }

    Value literalToValue(const SqlLiteral &literal)
    {
        Value v;
        if (literal.isString)
        {
            v.type = TYPE_STRING;
            v.s    = literal.stringValue;
        }
        else
        {
            v.type = TYPE_INT;
            v.i    = literal.intValue;
        }
        return v;
    }

    QueryResult makeError(const std::string &message)
    {
        QueryResult result;
        result.success = false;
        result.message = message;
        return result;
    }

    std::string normalizeType(const std::string &type)
    {
        std::string normalized = type;
        for (char &ch : normalized)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

        if (normalized == "STRING")
            normalized = "TEXT";

        if (normalized != "INT" && normalized != "TEXT")
            throw std::runtime_error("Unsupported column type: " + type);

        return normalized;
    }

    std::string valueTypeName(const Value &value)
    {
        return value.type == TYPE_INT ? "INT" : "TEXT";
    }

    bool valueMatchesColumn(const Value &value, const ColumnMetadata &column)
    {
        if (column.type == "INT")  return value.type == TYPE_INT;
        if (column.type == "TEXT") return value.type == TYPE_STRING;
        return false;
    }

    std::vector<std::string> columnNames(const TableMetadata &tm)
    {
        std::vector<std::string> names;
        names.reserve(tm.columns.size());
        for (const auto &column : tm.columns)
            names.push_back(column.name);
        return names;
    }

    std::vector<std::string> prefixedColumnNames(const TableMetadata &tm,
                                                  const std::string &tableAlias)
    {
        std::vector<std::string> names;
        names.reserve(tm.columns.size());
        for (const auto &column : tm.columns)
            names.push_back(tableAlias + "." + column.name);
        return names;
    }

    Record projectRecord(const Record &record, const TableMetadata &tm,
                         const std::vector<int> &indices)
    {
        Record projected;
        for (int index : indices)
        {
            projected.cols.push_back(tm.columns[index].name);
            if (index >= 0 && index < static_cast<int>(record.vals.size()))
                projected.vals.push_back(record.vals[index]);
        }
        return projected;
    }

    Record projectRecordCombined(const Record &record,
                                 const std::vector<int> &indices,
                                 const std::vector<std::string> &columnNames)
    {
        Record projected;
        projected.cols = columnNames;
        for (int index : indices)
        {
            if (index >= 0 && index < static_cast<int>(record.vals.size()))
                projected.vals.push_back(record.vals[index]);
        }
        return projected;
    }

    bool compareValues(const Value &left, const std::string &op, const Value &right)
    {
        if (left.type != right.type)
            return false;

        if (op == "BETWEEN")
            return false;

        if (left.type == TYPE_INT)
        {
            if (op == "=")  return left.i == right.i;
            if (op == "!=" || op == "<>") return left.i != right.i;
            if (op == "<")  return left.i < right.i;
            if (op == "<=") return left.i <= right.i;
            if (op == ">")  return left.i > right.i;
            if (op == ">=") return left.i >= right.i;
            return false;
        }

        if (op == "=")  return left.s == right.s;
        if (op == "!=" || op == "<>") return left.s != right.s;
        if (op == "<")  return left.s < right.s;
        if (op == "<=") return left.s <= right.s;
        if (op == ">")  return left.s > right.s;
        if (op == ">=") return left.s >= right.s;
        return false;
    }

    bool matchesWhereClause(const Record &rec, const TableMetadata &tm,
                            const SqlWhereClause &where)
    {
        if (!where.enabled)
            return true;

        int colIdx = -1;
        for (size_t i = 0; i < tm.columns.size(); ++i)
        {
            if (tm.columns[i].name == where.column)
            {
                colIdx = static_cast<int>(i);
                break;
            }
        }

        if (colIdx < 0 || colIdx >= static_cast<int>(rec.vals.size()))
            return false;

        const Value &value = rec.vals[colIdx];

        if (where.op == "BETWEEN")
        {
            if (!where.hasSecondValue)
                return false;

            Value lower = literalToValue(where.value);
            Value upper = literalToValue(where.secondValue);

            if (value.type != lower.type || value.type != upper.type)
                return false;

            if (value.type == TYPE_INT)
                return value.i >= lower.i && value.i <= upper.i;

            return value.s >= lower.s && value.s <= upper.s;
        }

        Value target = literalToValue(where.value);
        return compareValues(value, where.op, target);
    }

    bool tryGetPrimaryKeyValue(const TableMetadata &tm, const SqlWhereClause &where,
                               Value &pkValue)
    {
        if (!where.enabled || where.op != "=")
            return false;

        if (!where.tableName.empty() && where.tableName != tm.name)
            return false;

        int pkIndex = tm.primaryKeyIndex;
        if (pkIndex < 0 || pkIndex >= static_cast<int>(tm.columns.size()))
            return false;

        if (tm.columns[pkIndex].name != where.column)
            return false;

        pkValue = literalToValue(where.value);
        return true;
    }

    bool tryGetIndexedNameIds(StorageManager &storageManager, const TableMetadata &tm,
                              const SqlWhereClause &where, std::vector<int> &ids)
    {
        if (!where.enabled || where.column != "name" || !where.value.isString)
            return false;

        if (!where.tableName.empty() && where.tableName != tm.name)
            return false;

        int nameIndex = -1;
        for (size_t i = 0; i < tm.columns.size(); i++)
        {
            if (tm.columns[i].name == "name")
            {
                nameIndex = static_cast<int>(i);
                break;
            }
        }

        if (nameIndex < 0)
            return false;

        int pkIndex = tm.primaryKeyIndex >= 0 ? tm.primaryKeyIndex : 0;
        if (pkIndex < 0 || pkIndex >= static_cast<int>(tm.columns.size()) ||
            tm.columns[pkIndex].type != "INT")
            return false;

        if (where.op == "=")
        {
            ids = storageManager.getIndex().findAll(where.value.stringValue);
            return true;
        }

        if (where.op == "BETWEEN" && where.hasSecondValue && where.secondValue.isString)
        {
            ids = storageManager.getIndex().findRange(where.value.stringValue,
                                                      where.secondValue.stringValue);
            return true;
        }

        if (where.op == ">" || where.op == ">=")
        {
            ids = storageManager.getIndex().findRange(where.value.stringValue,
                                                      std::string(64, '\x7f'));
            return true;
        }

        if (where.op == "<" || where.op == "<=")
        {
            ids = storageManager.getIndex().findRange("", where.value.stringValue);
            return true;
        }

        return false;
    }

    bool parseQualifiedIdentifier(const std::string &token,
                                  std::string &tableName,
                                  std::string &columnName)
    {
        size_t pos = token.find('.');
        if (pos == std::string::npos)
        {
            tableName.clear();
            columnName = token;
            return false;
        }
        tableName = token.substr(0, pos);
        columnName = token.substr(pos + 1);
        return true;
    }

    int findColumnIndexInCombined(const std::vector<std::string> &combinedColumns,
                                  const std::string &expression)
    {
        std::string normalized = expression;
        for (char &ch : normalized) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));

        // Exact match first.
        for (size_t i = 0; i < combinedColumns.size(); ++i)
        {
            std::string candidate = combinedColumns[i];
            for (char &ch : candidate) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (candidate == normalized)
                return static_cast<int>(i);
        }

        // If the expression is unqualified, try first matching unqualified column.
        size_t dotPos = normalized.find('.');
        if (dotPos == std::string::npos)
        {
            for (size_t i = 0; i < combinedColumns.size(); ++i)
            {
                std::string candidate = combinedColumns[i];
                size_t pos = candidate.find('.');
                if (pos != std::string::npos)
                    candidate = candidate.substr(pos + 1);
                if (candidate == normalized)
                    return static_cast<int>(i);
            }
        }

        return -1;
    }

    bool matchesWhereClauseCombined(const Record &rec,
                                   const std::vector<std::string> &combinedColumns,
                                   const SqlWhereClause &where)
    {
        if (!where.enabled)
            return true;

        std::string lookup = where.column;
        if (!where.tableName.empty())
            lookup = where.tableName + "." + where.column;

        int idx = findColumnIndexInCombined(combinedColumns, lookup);
        if (idx < 0 || idx >= static_cast<int>(rec.vals.size()))
            return false;

        const Value &value = rec.vals[idx];
        Value target = literalToValue(where.value);
        return compareValues(value, where.op, target);
    }
}

Executor::Executor(const std::string &dataDir)
{
    storageManager = std::make_unique<StorageManager>(dataDir);
}

const Catalog &Executor::getCatalog() const
{
    return storageManager->getCatalog();
}

std::vector<std::string> Executor::listTables() const
{
    return storageManager->getCatalog().listTables();
}

void Executor::shutdown()
{
    // Roll back any uncommitted transaction so we never checkpoint dirty data.
    if (inTransaction)
    {
        try { storageManager->rollbackTransaction(); }
        catch (...) {}
        inTransaction = false;
    }
    storageManager->save();
}

QueryResult Executor::execute(const SqlStatement &statement)
{
    try
    {
        switch (statement.type)
        {
        case SqlStatementType::CreateTable: return executeCreateTable(statement);
        case SqlStatementType::Insert:      return executeInsert(statement);
        case SqlStatementType::Select:      return executeSelect(statement);
        case SqlStatementType::Update:      return executeUpdate(statement);
        case SqlStatementType::Delete:      return executeDelete(statement);
        case SqlStatementType::Begin:       return executeBegin(statement);
        case SqlStatementType::Commit:      return executeCommit(statement);
        case SqlStatementType::Rollback:    return executeRollback(statement);
        default:
        {
            QueryResult result;
            result.success = false;
            result.message = "Unknown statement type";
            return result;
        }
        }
    }
    catch (const std::exception &ex)
    {
        QueryResult result;
        result.success = false;
        result.message = ex.what();
        return result;
    }
}

QueryResult Executor::executeCreateTable(const SqlStatement &statement)
{
    if (statement.columns.empty())
        return makeError("CREATE TABLE requires at least one column");

    std::vector<ColumnMetadata> columns;
    std::set<std::string> seenColumns;
    bool hasPrimaryKey = false;

    for (size_t i = 0; i < statement.columns.size(); i++)
    {
        ColumnMetadata col;
        col.name        = statement.columns[i].name;
        col.type        = normalizeType(statement.columns[i].type);
        col.isPrimaryKey = statement.columns[i].isPrimaryKey;
        col.isNotNull   = statement.columns[i].isNotNull || col.isPrimaryKey;

        if (!seenColumns.insert(col.name).second)
            return makeError("Duplicate column: " + col.name);

        if (col.isPrimaryKey)
        {
            if (hasPrimaryKey)
                return makeError("Only one PRIMARY KEY column is supported");
            hasPrimaryKey = true;
        }

        columns.push_back(col);
    }

    if (!hasPrimaryKey)
    {
        columns[0].isPrimaryKey = true;
        columns[0].isNotNull    = true;
    }

    storageManager->createTable(statement.tableName, columns);

    QueryResult result;
    result.success     = true;
    result.message     = "Table '" + statement.tableName + "' created successfully";
    result.rowsAffected = 0;
    return result;
}

Value Executor::sqlLiteralToValue(const SqlLiteral &literal)
{
    return literalToValue(literal);
}

Record Executor::buildRecordFromValues(const TableMetadata &tm,
                                       const std::vector<Value> &values)
{
    Record rec;
    rec.vals = values;
    for (const auto &col : tm.columns)
        rec.cols.push_back(col.name);
    return rec;
}

QueryResult Executor::executeInsert(const SqlStatement &statement)
{
    if (!storageManager->getCatalog().tableExists(statement.tableName))
        return makeError("Table not found: " + statement.tableName);

    TableMetadata tm = storageManager->getCatalog().getTable(statement.tableName);

    std::vector<Value> values(tm.columns.size());
    std::vector<bool> assigned(tm.columns.size(), false);

    if (statement.insertColumns.empty())
    {
        if (statement.insertValues.size() != tm.columns.size())
            return makeError("Value count mismatch");

        for (size_t i = 0; i < statement.insertValues.size(); i++)
        {
            values[i]   = sqlLiteralToValue(statement.insertValues[i]);
            assigned[i] = true;
        }
    }
    else
    {
        if (statement.insertValues.size() != statement.insertColumns.size())
            return makeError("Column/value count mismatch");

        for (size_t i = 0; i < statement.insertColumns.size(); i++)
        {
            int columnIndex = storageManager->getCatalog().getColumnIndex(
                statement.tableName, statement.insertColumns[i]);

            if (assigned[columnIndex])
                return makeError("Duplicate INSERT column: " + statement.insertColumns[i]);

            values[columnIndex]   = sqlLiteralToValue(statement.insertValues[i]);
            assigned[columnIndex] = true;
        }

        for (size_t i = 0; i < assigned.size(); i++)
        {
            if (!assigned[i])
                return makeError(
                    "INSERT column list must include every column; "
                    "defaults and NULL are not supported");
        }
    }

    for (size_t i = 0; i < values.size(); i++)
    {
        if (!valueMatchesColumn(values[i], tm.columns[i]))
        {
            return makeError("Type mismatch for column '" + tm.columns[i].name +
                             "': expected " + tm.columns[i].type +
                             ", got " + valueTypeName(values[i]));
        }
    }

    Record rec = buildRecordFromValues(tm, values);
    int tableId = storageManager->getTableId(statement.tableName);
    storageManager->insertRecord(tableId, statement.tableName, rec);

    QueryResult result;
    result.success     = true;
    result.message     = "1 row inserted";
    result.rowsAffected = 1;
    return result;
}

QueryResult Executor::executeBegin(const SqlStatement &statement)
{
    (void)statement;
    QueryResult result;
    try
    {
        storageManager->beginTransaction();
        inTransaction = true;
        result.success = true;
        result.message = "Transaction started";
    }
    catch (const std::exception &ex)
    {
        result.success = false;
        result.message = ex.what();
    }
    return result;
}

QueryResult Executor::executeCommit(const SqlStatement &statement)
{
    (void)statement;
    QueryResult result;
    try
    {
        if (!inTransaction)
        {
            result.success = false;
            result.message = "No transaction in progress";
            return result;
        }

        storageManager->commitTransaction();
        inTransaction = false;
        result.success = true;
        result.message = "Transaction committed";
    }
    catch (const std::exception &ex)
    {
        result.success = false;
        result.message = ex.what();
    }
    return result;
}

QueryResult Executor::executeRollback(const SqlStatement &statement)
{
    (void)statement;
    QueryResult result;
    try
    {
        if (!inTransaction)
        {
            result.success = false;
            result.message = "No transaction in progress";
            return result;
        }

        storageManager->rollbackTransaction();
        inTransaction = false;
        result.success = true;
        result.message = "Transaction rolled back";
    }
    catch (const std::exception &ex)
    {
        result.success = false;
        result.message = ex.what();
    }
    return result;
}

QueryResult Executor::executeSelect(const SqlStatement &statement)
{
    if (!storageManager->getCatalog().tableExists(statement.tableName))
        return makeError("Table not found: " + statement.tableName);

    TableMetadata tm     = storageManager->getCatalog().getTable(statement.tableName);
    int tableId          = storageManager->getTableId(statement.tableName);
    Table table          = makeTableFromMetadata(tm, tableId);

    std::vector<Record> results;
    std::vector<int>    projection;
    std::vector<std::string> outputColumns;

    if (statement.selectAll)
    {
        if (statement.hasJoin)
        {
            TableMetadata joinTm = storageManager->getCatalog().getTable(statement.joinTable);
            outputColumns = prefixedColumnNames(tm, statement.tableName);
            std::vector<std::string> joinNames = prefixedColumnNames(joinTm, statement.joinTable);
            outputColumns.insert(outputColumns.end(), joinNames.begin(), joinNames.end());
            for (size_t i = 0; i < tm.columns.size() + joinTm.columns.size(); i++)
                projection.push_back(static_cast<int>(i));
        }
        else
        {
            outputColumns = columnNames(tm);
            for (size_t i = 0; i < tm.columns.size(); i++)
                projection.push_back(static_cast<int>(i));
        }
    }
    else
    {
        for (const auto &column : statement.selectColumns)
        {
            if (statement.hasJoin)
            {
                std::string tableAlias, columnName;
                parseQualifiedIdentifier(column, tableAlias, columnName);
                if (tableAlias.empty() || tableAlias == statement.tableName)
                {
                    int columnIndex = storageManager->getCatalog().getColumnIndex(statement.tableName, columnName);
                    projection.push_back(static_cast<int>(columnIndex));
                    outputColumns.push_back(statement.tableName + "." + columnName);
                }
                else if (tableAlias == statement.joinTable)
                {
                    TableMetadata joinTm = storageManager->getCatalog().getTable(statement.joinTable);
                    int columnIndex = storageManager->getCatalog().getColumnIndex(statement.joinTable, columnName);
                    projection.push_back(static_cast<int>(tm.columns.size() + columnIndex));
                    outputColumns.push_back(statement.joinTable + "." + columnName);
                }
                else
                {
                    return makeError("Unknown table in SELECT list: " + tableAlias);
                }
            }
            else
            {
                int columnIndex = storageManager->getCatalog().getColumnIndex(
                    statement.tableName, column);
                projection.push_back(columnIndex);
                outputColumns.push_back(tm.columns[columnIndex].name);
            }
        }
    }

    QueryResult result;
    result.indexUsed = false;

    // ----------------------------------------------------------------
    // JOIN path: nested-loop inner join, then project from combined row
    // ----------------------------------------------------------------
    if (statement.hasJoin)
    {
        if (!storageManager->getCatalog().tableExists(statement.joinTable))
            return makeError("Table not found: " + statement.joinTable);

        TableMetadata joinTm  = storageManager->getCatalog().getTable(statement.joinTable);
        int joinTableId       = storageManager->getTableId(statement.joinTable);
        Table joinTable       = makeTableFromMetadata(joinTm, joinTableId);

        // Parse "table.column" join keys (sql parser stores them in joinLeftColumn etc.)
        std::string leftTbl, leftCol, rightTbl, rightCol;
        parseQualifiedIdentifier(statement.joinLeftColumn,  leftTbl,  leftCol);
        parseQualifiedIdentifier(statement.joinRightColumn, rightTbl, rightCol);

        // If unqualified, assume left=primary table, right=join table
        if (leftTbl.empty())  leftTbl  = statement.tableName;
        if (rightTbl.empty()) rightTbl = statement.joinTable;

        // Resolve which index in the combined record each join key lives at
        int leftJoinIdx  = storageManager->getCatalog().getColumnIndex(leftTbl,  leftCol);
        int rightJoinIdx = storageManager->getCatalog().getColumnIndex(rightTbl, rightCol);

        // If user wrote them in reverse order (right ON left), swap
        if (leftTbl == statement.joinTable)
        {
            std::swap(leftJoinIdx,  rightJoinIdx);
            std::swap(leftTbl,      rightTbl);
        }

        // Build combined column name list for WHERE resolution
        std::vector<std::string> combinedColumns =
            prefixedColumnNames(tm, statement.tableName);
        {
            std::vector<std::string> jc = prefixedColumnNames(joinTm, statement.joinTable);
            combinedColumns.insert(combinedColumns.end(), jc.begin(), jc.end());
        }

        std::vector<Record> leftRows  = fullTableScan(storageManager->getKVStore(), table);
        std::vector<Record> rightRows = fullTableScan(storageManager->getKVStore(), joinTable);

        std::vector<Record> joinedRows;
        for (const auto &lr : leftRows)
        {
            if (leftJoinIdx >= static_cast<int>(lr.vals.size())) continue;
            for (const auto &rr : rightRows)
            {
                if (rightJoinIdx >= static_cast<int>(rr.vals.size())) continue;
                if (!compareValues(lr.vals[leftJoinIdx], "=", rr.vals[rightJoinIdx]))
                    continue;

                // Combine left and right into one record
                Record combined;
                combined.cols = combinedColumns;
                combined.vals.insert(combined.vals.end(), lr.vals.begin(), lr.vals.end());
                combined.vals.insert(combined.vals.end(), rr.vals.begin(), rr.vals.end());

                if (statement.where.enabled &&
                    !matchesWhereClauseCombined(combined, combinedColumns, statement.where))
                    continue;

                joinedRows.push_back(combined);
            }
        }

        // Project the requested columns out of each combined row
        std::vector<Record> projectedRows;
        projectedRows.reserve(joinedRows.size());
        for (const auto &rec : joinedRows)
            projectedRows.push_back(projectRecordCombined(rec, projection, outputColumns));

        result.success      = true;
        result.columns      = outputColumns;
        result.rows         = projectedRows;
        result.message      = std::to_string(projectedRows.size()) + " rows selected";
        result.rowsAffected = static_cast<int>(projectedRows.size());
        return result;
    }

    // ----------------------------------------------------------------
    // Non-JOIN path (unchanged)
    // ----------------------------------------------------------------
    if (statement.where.enabled)
        storageManager->getCatalog().getColumnIndex(statement.tableName,
                                                    statement.where.column);

    if (statement.where.enabled)
    {
        Value pkValue;
        if (tryGetPrimaryKeyValue(tm, statement.where, pkValue))
        {
            std::vector<char> key = encodeKey(table.tableId, {pkValue});
            std::vector<char> data = storageManager->getKVStore().get(key);
            if (!data.empty())
            {
                Record rec = deserialize(data);
                if (matchesWhereClause(rec, tm, statement.where))
                    results.push_back(rec);
            }
        }
        else
        {
            std::vector<int> indexedIds;
            if (tryGetIndexedNameIds(*storageManager, tm, statement.where, indexedIds))
            {
                result.indexUsed = true;
                for (int id : indexedIds)
                {
                    Value pk;
                    pk.type = TYPE_INT;
                    pk.i    = id;

                    std::vector<char> key  = encodeKey(table.tableId, {pk});
                    std::vector<char> data = storageManager->getKVStore().get(key);
                    if (data.empty())
                        continue;

                    Record rec = deserialize(data);
                    if (matchesWhereClause(rec, tm, statement.where))
                        results.push_back(rec);
                }
            }
            else
            {
                std::vector<Record> allRecords =
                    fullTableScan(storageManager->getKVStore(), table);
                for (const auto &rec : allRecords)
                {
                    if (matchesWhereClause(rec, tm, statement.where))
                        results.push_back(rec);
                }
            }
        }
    }
    else
    {
        results = fullTableScan(storageManager->getKVStore(), table);
    }

    std::vector<Record> projectedRows;
    projectedRows.reserve(results.size());
    for (const auto &record : results)
        projectedRows.push_back(projectRecord(record, tm, projection));

    result.success     = true;
    result.columns     = outputColumns;
    result.rows        = projectedRows;
    result.message     = std::to_string(projectedRows.size()) + " rows selected";
    result.rowsAffected = static_cast<int>(projectedRows.size());
    return result;
}

QueryResult Executor::executeUpdate(const SqlStatement &statement)
{
    if (!storageManager->getCatalog().tableExists(statement.tableName))
        return makeError("Table not found: " + statement.tableName);

    if (!statement.hasUpdateAssignment || statement.updateAssignments.empty())
        return makeError("UPDATE requires a SET assignment");

    TableMetadata tm = storageManager->getCatalog().getTable(statement.tableName);
    int tableId      = storageManager->getTableId(statement.tableName);
    int targetColumn = storageManager->getCatalog().getColumnIndex(
        statement.tableName, statement.updateAssignments[0]);

    if (targetColumn == tm.primaryKeyIndex)
        return makeError("Updating the primary key is not supported");

    if (statement.where.enabled)
        storageManager->getCatalog().getColumnIndex(statement.tableName,
                                                    statement.where.column);

    Value newValue = sqlLiteralToValue(statement.updateValue);
    if (!valueMatchesColumn(newValue, tm.columns[targetColumn]))
    {
        return makeError("Type mismatch for column '" + tm.columns[targetColumn].name +
                         "': expected " + tm.columns[targetColumn].type +
                         ", got " + valueTypeName(newValue));
    }

    int rowsAffected = 0;
    Table table = makeTableFromMetadata(tm, tableId);

    Value pkValue;
    if (tryGetPrimaryKeyValue(tm, statement.where, pkValue))
    {
        std::vector<char> key  = encodeKey(table.tableId, {pkValue});
        std::vector<char> data = storageManager->getKVStore().get(key);

        if (!data.empty())
        {
            Record rec = deserialize(data);
            if (targetColumn < static_cast<int>(rec.vals.size()))
            {
                Record updated = rec;
                updated.vals[targetColumn] = newValue;
                storageManager->updateRecord(tableId, rec, updated);
                rowsAffected = 1;
            }
        }
    }
    else
    {
        std::vector<Record> allRecords =
            fullTableScan(storageManager->getKVStore(), table);

        for (const auto &rec : allRecords)
        {
            if (!matchesWhereClause(rec, tm, statement.where))
                continue;

            if (targetColumn < 0 || targetColumn >= static_cast<int>(rec.vals.size()))
                continue;

            Record updated = rec;
            updated.vals[targetColumn] = newValue;
            storageManager->updateRecord(tableId, rec, updated);
            ++rowsAffected;
        }
    }

    QueryResult result;
    result.success     = true;
    result.message     = std::to_string(rowsAffected) + " row(s) updated";
    result.rowsAffected = rowsAffected;
    return result;
}

QueryResult Executor::executeDelete(const SqlStatement &statement)
{
    if (!storageManager->getCatalog().tableExists(statement.tableName))
        return makeError("Table not found: " + statement.tableName);

    TableMetadata tm = storageManager->getCatalog().getTable(statement.tableName);
    int tableId      = storageManager->getTableId(statement.tableName);
    int rowsAffected = 0;

    if (statement.where.enabled)
        storageManager->getCatalog().getColumnIndex(statement.tableName,
                                                    statement.where.column);

    Table table = makeTableFromMetadata(tm, tableId);

    Value pkValue;
    if (tryGetPrimaryKeyValue(tm, statement.where, pkValue))
    {
        std::vector<char> key  = encodeKey(table.tableId, {pkValue});
        std::vector<char> data = storageManager->getKVStore().get(key);

        if (!data.empty())
        {
            Record rec = deserialize(data);
            storageManager->deleteRecord(tableId, rec);
            rowsAffected = 1;
        }
    }
    else
    {
        std::vector<Record> allRecords =
            fullTableScan(storageManager->getKVStore(), table);

        for (const auto &rec : allRecords)
        {
            if (!matchesWhereClause(rec, tm, statement.where))
                continue;

            storageManager->deleteRecord(tableId, rec);
            ++rowsAffected;
        }
    }

    QueryResult result;
    result.success     = true;
    result.message     = std::to_string(rowsAffected) + " row(s) deleted";
    result.rowsAffected = rowsAffected;
    return result;
}