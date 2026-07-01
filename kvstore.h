#ifndef KVSTORE_H
#define KVSTORE_H

#include <shared_mutex>
#include <utility>
#include <vector>

#include "btree.h"

class KVStore
{
private:
    Bplustree tree;
    mutable std::shared_mutex treeMutex;

public:
    void clear();
    void put(const std::vector<char> &key, const std::vector<char> &value);
    std::vector<char> get(const std::vector<char> &key);
    void del(const std::vector<char> &key);
    std::vector<std::vector<char>> scanPrefix(const std::vector<char> &prefix);
    std::vector<std::pair<std::vector<char>, std::vector<char>>> scanPrefixKV(const std::vector<char> &prefix);
    std::vector<std::pair<std::vector<char>, std::vector<char>>> scanRange(
        const std::vector<char> &start,
        const std::vector<char> &end);
};

#endif
