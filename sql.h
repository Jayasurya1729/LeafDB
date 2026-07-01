#ifndef SQL_H
#define SQL_H

#include <string>
#include <vector>

enum class SqlStatementType
{
    Invalid,
    CreateTable,
    Insert,
    Select,
    Update,
    Delete,
    Begin,
    Commit,
    Rollback
};

struct SqlLiteral
{
    bool isString = false;
    std::string stringValue;
    int intValue = 0;
};

struct SqlColumnDefinition
{
    std::string name;
    std::string type;
    bool isPrimaryKey = false;
    bool isNotNull = false;
};

struct SqlWhereClause
{
    std::string tableName;
    std::string column;
    std::string op;
    SqlLiteral value;
    SqlLiteral secondValue;
    bool hasSecondValue = false;
    bool enabled = false;
};

struct SqlStatement
{
    SqlStatementType type = SqlStatementType::Invalid;
    std::string tableName;
    bool hasJoin = false;
    std::string joinTable;
    std::string joinLeftColumn;
    std::string joinRightColumn;

    std::vector<SqlColumnDefinition> columns;

    std::vector<std::string> selectColumns;
    bool selectAll = false;

    std::vector<std::string> insertColumns;
    std::vector<SqlLiteral> insertValues;

    std::vector<std::string> updateAssignments;
    SqlLiteral updateValue;
    bool hasUpdateAssignment = false;

    SqlWhereClause where;
};

std::vector<std::string> tokenizeSql(const std::string &sql);
SqlStatement parseSql(const std::string &sql);

#endif // SQL_H
