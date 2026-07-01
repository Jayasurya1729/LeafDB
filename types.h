#ifndef TYPES_H
#define TYPES_H

#include <string>
#include <vector>

enum Type
{
    TYPE_INT = 1,
    TYPE_STRING = 2
};

struct Value
{
    int type = TYPE_INT;
    int i = 0;
    std::string s;
};

struct Record
{
    std::vector<std::string> cols;
    std::vector<Value> vals;
};

#endif
