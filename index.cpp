#include "index.h"
#include "encode.h"

namespace
{
    // Index key layout: [0,0,0,2][name bytes][0x00][id big-endian 4 bytes]
    // The leading 0,0,0,2 tag separates index keys from data keys in the same KVStore.
    std::vector<char> encodeIndexKey(const std::string &name, int id)
    {
        std::vector<char> key;
        appendInt(key, 2);
        key.insert(key.end(), name.begin(), name.end());
        key.push_back('\0');
        appendInt(key, id);
        return key;
    }

    std::vector<char> encodeIndexPrefix(const std::string &name)
    {
        std::vector<char> key;
        appendInt(key, 2);
        key.insert(key.end(), name.begin(), name.end());
        key.push_back('\0');
        return key;
    }

    std::string decodeIndexedName(const std::vector<char> &key)
    {
        // Layout: [4-byte tag][name][0x00][4-byte id] => name starts at offset 4
        if (key.size() < 6)
            return {};

        size_t offset = 4;
        size_t end    = offset;
        while (end < key.size() && key[end] != '\0')
            ++end;

        if (end >= key.size())
            return {};

        return std::string(key.begin() + offset, key.begin() + end);
    }

    int decodeIndexedId(const std::vector<char> &key)
    {
        // Last 4 bytes are the big-endian id.
        if (key.size() < 8)
            return 0;

        int id = 0;
        size_t offset = key.size() - 4;
        for (int i = 0; i < 4; i++)
            id = (id << 8) | static_cast<unsigned char>(key[offset + i]);
        return id;
    }
}

void Index::clear()
{
    kv.clear();
}

void Index::insert(const std::string &name, int id)
{
    std::vector<char> key = encodeIndexKey(name, id);
    kv.put(key, {});
}

std::vector<int> Index::findAll(const std::string &name)
{
    std::vector<char> prefix = encodeIndexPrefix(name);
    auto keys = kv.scanPrefix(prefix);

    std::vector<int> result;
    result.reserve(keys.size());
    for (auto &k : keys)
        result.push_back(decodeIndexedId(k));
    return result;
}

std::vector<int> Index::findRange(const std::string &start, const std::string &end)
{
    std::vector<char> lower = encodeIndexPrefix(start);
    // Upper bound: use the largest possible id so the range scan covers all ids
    // for names up to and including 'end'.
    std::vector<char> upper = encodeIndexKey(end, 0x7fffffff);

    auto entries = kv.scanRange(lower, upper);

    std::vector<int> result;
    for (const auto &entry : entries)
    {
        std::string indexedName = decodeIndexedName(entry.first);
        if (indexedName.empty() || indexedName < start || indexedName > end)
            continue;
        result.push_back(decodeIndexedId(entry.first));
    }
    return result;
}

void Index::remove(const std::string &name, int id)
{
    std::vector<char> key = encodeIndexKey(name, id);
    kv.del(key);
}