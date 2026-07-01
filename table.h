#ifndef TABLE_H
#define TABLE_H

#include <string>
#include <vector>

#include "index.h"
#include "record.h"

struct Table
{
    std::string name;
    int tableId = 1;
    std::vector<std::string> cols;
    std::vector<int> types;
    int primaryKeyIndex = 0;
};

void insertRow(KVStore &kv, Table &t, Record &rec, Index &idx);
Record getRow(KVStore &kv, Table &t, int id);
std::vector<Record> findByName(KVStore &kv, Table &t, Index &idx, const std::string &name);
std::vector<Record> fullTableScan(KVStore &kv, Table &t);
std::vector<Record> filterByNameScan(KVStore &kv, Table &t, const std::string &name);
std::vector<Record> filterByName(KVStore &kv, Table &t, Index *idx, const std::string &name);
std::vector<Record> rangeQueryID(KVStore &kv, Table &t, int startID, int endID);
std::vector<Record> rangeQueryName(KVStore &kv, Table &t, std::string start, std::string end);
void deleteRow(KVStore &kv, Table &t, Index &idx, int id);
void updateRow(KVStore &kv, Table &t, Index &idx, int id, const std::string &newName);

#endif
