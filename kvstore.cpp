#include "kvstore.h"
#include <mutex>

void KVStore::clear()
{
    std::unique_lock<std::shared_mutex> lock(treeMutex);
    tree.clear();
}

void KVStore::put(const std::vector<char> &key, const std::vector<char> &value)
{
    std::unique_lock<std::shared_mutex> lock(treeMutex);
    tree.insert(key, value);
}

// get() is a read — use shared_lock so concurrent reads don't block each other.
std::vector<char> KVStore::get(const std::vector<char> &key)
{
    std::shared_lock<std::shared_mutex> lock(treeMutex);
    return tree.search(key);
}

void KVStore::del(const std::vector<char> &key)
{
    std::unique_lock<std::shared_mutex> lock(treeMutex);
    tree.remove(key);
}

std::vector<std::vector<char>> KVStore::scanPrefix(const std::vector<char> &prefix)
{
    std::shared_lock<std::shared_mutex> lock(treeMutex);
    return tree.scanPrefix(prefix);
}

std::vector<std::pair<std::vector<char>, std::vector<char>>>
KVStore::scanPrefixKV(const std::vector<char> &prefix)
{
    std::shared_lock<std::shared_mutex> lock(treeMutex);
    return tree.scanPrefixKV(prefix);
}

std::vector<std::pair<std::vector<char>, std::vector<char>>>
KVStore::scanRange(const std::vector<char> &start, const std::vector<char> &end)
{
    std::shared_lock<std::shared_mutex> lock(treeMutex);
    return tree.scanRange(start, end);
}