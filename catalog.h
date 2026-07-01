#ifndef CATALOG_H
#define CATALOG_H

#include <string>
#include <vector>
#include <map>

struct ColumnMetadata
{
    std::string name;
    std::string type;  // "INT" or "TEXT"
    bool isPrimaryKey = false;
    bool isNotNull = false;
};

struct TableMetadata
{
    std::string name;
    std::vector<ColumnMetadata> columns;
    int primaryKeyIndex = -1;
};

class Catalog
{
private:
    std::map<std::string, TableMetadata> tables;

public:
    Catalog() = default;

    void createTable(const std::string &tableName, const std::vector<ColumnMetadata> &columns);
    
    bool tableExists(const std::string &tableName) const;
    
    TableMetadata getTable(const std::string &tableName) const;

    std::vector<std::string> listTables() const;
    
    int getColumnIndex(const std::string &tableName, const std::string &columnName) const;
};

#endif // CATALOG_H
