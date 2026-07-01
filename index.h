#ifndef INDEX_H
#define INDEX_H

#include <string>
#include <vector>

#include "kvstore.h"

class Index
{
public:
    KVStore kv;

    void clear();
    void insert(const std::string &name, int id);
    std::vector<int> findAll(const std::string &name);
    std::vector<int> findRange(const std::string &start, const std::string &end);
    void remove(const std::string &name, int id);
};

#endif
