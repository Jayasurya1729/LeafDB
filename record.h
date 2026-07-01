#ifndef RECORD_H
#define RECORD_H

#include <vector>

#include "types.h"

std::vector<char> serialize(const Record &rec);
Record deserialize(const std::vector<char> &data);

#endif
