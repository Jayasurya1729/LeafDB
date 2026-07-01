#include "catalog.h"
#include <stdexcept>

namespace
{
    std::string lowerCopy(const std::string &value)
    {
        std::string result = value;
        for (char &ch : result)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return result;
    }
}

void Catalog::createTable(const std::string &tableName, const std::vector<ColumnMetadata> &columns)
{
    std::string normalizedTableName = lowerCopy(tableName);

    if (tables.find(normalizedTableName) != tables.end())
    {
        throw std::runtime_error("Table already exists: " + tableName);
    }

    TableMetadata tm;
    tm.name = normalizedTableName;
    tm.columns = columns;

    for (size_t i = 0; i < columns.size(); i++)
    {
        if (columns[i].isPrimaryKey)
        {
            tm.primaryKeyIndex = i;
            break;
        }
    }

    for (auto &column : tm.columns)
        column.name = lowerCopy(column.name);

    tables[normalizedTableName] = tm;
}

bool Catalog::tableExists(const std::string &tableName) const
{
    return tables.find(lowerCopy(tableName)) != tables.end();
}

TableMetadata Catalog::getTable(const std::string &tableName) const
{
    auto it = tables.find(lowerCopy(tableName));
    if (it == tables.end())
    {
        throw std::runtime_error("Table not found: " + tableName);
    }
    return it->second;
}

std::vector<std::string> Catalog::listTables() const
{
    std::vector<std::string> names;
    names.reserve(tables.size());
    for (const auto &entry : tables)
        names.push_back(entry.first);
    return names;
}

int Catalog::getColumnIndex(const std::string &tableName, const std::string &columnName) const
{
    TableMetadata tm = getTable(tableName);
    std::string normalizedColumnName = lowerCopy(columnName);
    for (size_t i = 0; i < tm.columns.size(); i++)
    {
        if (tm.columns[i].name == normalizedColumnName)
        {
            return i;
        }
    }
    throw std::runtime_error("Column not found: " + columnName);
}
