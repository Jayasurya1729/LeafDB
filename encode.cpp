#include "encode.h"

void appendInt(std::vector<char> &out, int x)
{
    for (int i = 3; i >= 0; i--)
    {
        out.push_back((x >> (i * 8)) & 0xFF);
    }
}

void appendString(std::vector<char> &out, const std::string &s)
{
    appendInt(out, static_cast<int>(s.size()));
    for (char c : s)
        out.push_back(c);
}

void appendValue(std::vector<char> &out, const Value &v)
{
    appendInt(out, v.type);

    if (v.type == TYPE_INT)
    {
        appendInt(out, v.i);
    }
    else
    {
        appendString(out, v.s);
    }
}

std::vector<char> encodeKey(int table_id, const std::vector<Value> &vals)
{
    std::vector<char> out;
    appendInt(out, table_id);

    for (const auto &v : vals)
    {
        appendValue(out, v);
    }

    return out;
}

std::vector<char> encodePrefix(int table_id)
{
    std::vector<char> out;
    appendInt(out, table_id);
    return out;
}
