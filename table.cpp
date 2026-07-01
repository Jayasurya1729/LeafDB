#include "table.h"

#include "encode.h"

void insertRow(KVStore &kv, Table &t, Record &rec, Index &idx)
{
    Value pk = rec.vals[t.primaryKeyIndex];
    int tableId = (t.tableId > 0) ? t.tableId : 1;

    std::vector<char> key = encodeKey(tableId, {pk});
    std::vector<char> value = serialize(rec);

    kv.put(key, value);

    // Index the "name" column (column 1) if it is a TEXT with an INT primary key.
    if (t.primaryKeyIndex == 0 &&
        rec.vals.size() > 1 &&
        rec.vals[1].type == TYPE_STRING &&
        pk.type == TYPE_INT)
    {
        idx.insert(rec.vals[1].s, pk.i);
    }
}

Record getRow(KVStore &kv, Table &t, int id)
{
    Value v;
    v.type = TYPE_INT;
    v.i = id;

    int tableId = (t.tableId > 0) ? t.tableId : 1;
    std::vector<char> key = encodeKey(tableId, {v});
    std::vector<char> data = kv.get(key);
    return deserialize(data);
}

std::vector<Record> findByName(KVStore &kv, Table &t, Index &idx, const std::string &name)
{
    std::vector<int> ids = idx.findAll(name);
    std::vector<Record> result;

    for (int id : ids)
    {
        Value v;
        v.type = TYPE_INT;
        v.i = id;

        int tableId = (t.tableId > 0) ? t.tableId : 1;
        std::vector<char> key = encodeKey(tableId, {v});
        std::vector<char> data = kv.get(key);

        if (data.empty())
            continue;

        result.push_back(deserialize(data));
    }

    return result;
}

std::vector<Record> fullTableScan(KVStore &kv, Table &t)
{
    std::vector<Record> result;

    int tableId = (t.tableId > 0) ? t.tableId : 1;

    // Use [encodePrefix(tableId), encodePrefix(tableId+1)) as the key range.
    // encodePrefix(N) produces the 4-byte big-endian N.  Any real data key
    // for table N is longer than 4 bytes so it compares strictly greater than
    // encodePrefix(N), and strictly less than encodePrefix(N+1) because the
    // first differing byte is N < N+1.  scanRange includes both endpoints via
    // cmp(key,end) > 0 to stop, so keys equal to encodePrefix(N+1) would be
    // included — but no real key is exactly 4 bytes, so this is safe.
    std::vector<char> startKey = encodePrefix(tableId);
    std::vector<char> endKey   = encodePrefix(tableId + 1);

    auto entries = kv.scanRange(startKey, endKey);

    for (auto &entry : entries)
    {
        if (entry.second.empty())
            continue;

        Record r = deserialize(entry.second);
        result.push_back(r);
    }

    return result;
}

std::vector<Record> filterByNameScan(KVStore &kv, Table &t, const std::string &name)
{
    std::vector<Record> all = fullTableScan(kv, t);
    std::vector<Record> result;

    for (auto &r : all)
    {
        if (r.vals.size() > 1 && r.vals[1].type == TYPE_STRING && r.vals[1].s == name)
            result.push_back(r);
    }

    return result;
}

std::vector<Record> filterByName(KVStore &kv, Table &t, Index *idx, const std::string &name)
{
    if (idx != nullptr)
        return findByName(kv, t, *idx, name);

    return filterByNameScan(kv, t, name);
}

std::vector<Record> rangeQueryID(KVStore &kv, Table &t, int startID, int endID)
{
    Value v1, v2;
    v1.type = TYPE_INT; v1.i = startID;
    v2.type = TYPE_INT; v2.i = endID;

    int tableId = (t.tableId > 0) ? t.tableId : 1;
    std::vector<char> startKey = encodeKey(tableId, {v1});
    std::vector<char> endKey   = encodeKey(tableId, {v2});

    auto entries = kv.scanRange(startKey, endKey);
    std::vector<Record> result;

    for (auto &entry : entries)
    {
        if (entry.second.empty())
            continue;
        result.push_back(deserialize(entry.second));
    }

    return result;
}

std::vector<Record> rangeQueryName(KVStore &kv, Table &t, std::string start, std::string end)
{
    (void)t;
    Value v1, v2;
    v1.type = TYPE_STRING; v1.s = start;
    v2.type = TYPE_STRING; v2.s = end;

    int tableId = (t.tableId > 0) ? t.tableId : 1;
    std::vector<char> startKey = encodeKey(tableId, {v1});
    std::vector<char> endKey   = encodeKey(tableId, {v2});

    auto keys = kv.scanRange(startKey, endKey);
    std::vector<Record> result;

    for (auto &k : keys)
    {
        // Last 4 bytes of the key encode the integer ID in big-endian.
        if (k.first.size() < 4)
            continue;

        int offset = static_cast<int>(k.first.size()) - 4;
        int id = 0;
        for (int i = 0; i < 4; i++)
            id = (id << 8) | static_cast<unsigned char>(k.first[offset + i]);

        Value v;
        v.type = TYPE_INT;
        v.i = id;

        std::vector<char> pk = encodeKey(tableId, {v});
        std::vector<char> data = kv.get(pk);

        if (data.empty())
            continue;

        result.push_back(deserialize(data));
    }

    return result;
}

void deleteRow(KVStore &kv, Table &t, Index &idx, int id)
{
    Value v;
    v.type = TYPE_INT;
    v.i = id;

    int tableId = (t.tableId > 0) ? t.tableId : 1;
    std::vector<char> key = encodeKey(tableId, {v});
    std::vector<char> data = kv.get(key);

    if (data.empty())
        return;

    Record r = deserialize(data);

    if (r.vals.size() > 1 && r.vals[1].type == TYPE_STRING)
        idx.remove(r.vals[1].s, id);

    kv.del(key);
}

void updateRow(KVStore &kv, Table &t, Index &idx, int id, const std::string &newName)
{
    Value v;
    v.type = TYPE_INT;
    v.i = id;

    int tableId = (t.tableId > 0) ? t.tableId : 1;
    std::vector<char> key = encodeKey(tableId, {v});
    std::vector<char> data = kv.get(key);

    if (data.empty())
        return;

    Record r = deserialize(data);

    if (r.vals.size() <= 1 || r.vals[1].type != TYPE_STRING)
        return;

    idx.remove(r.vals[1].s, id);
    r.vals[1].s = newName;

    kv.put(key, serialize(r));
    idx.insert(newName, id);
}