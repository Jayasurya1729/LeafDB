#ifndef ENCODE_H
#define ENCODE_H

#include <string>
#include <vector>

#include "types.h"

void appendInt(std::vector<char> &out, int x);
void appendString(std::vector<char> &out, const std::string &s);
void appendValue(std::vector<char> &out, const Value &v);
std::vector<char> encodeKey(int table_id, const std::vector<Value> &vals);
std::vector<char> encodePrefix(int table_id);

#endif
