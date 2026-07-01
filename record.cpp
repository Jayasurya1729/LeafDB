#include "record.h"

#include <cstring>

// Serialize a Record to bytes using a fixed little-endian layout.
// Layout: [count:4][type:4][value...] per value
//   INT  value: [i:4]
//   TEXT value: [len:4][bytes...]
// All integers written little-endian via memcpy (matches the host).
std::vector<char> serialize(const Record &rec)
{
    std::vector<char> out;

    int n = static_cast<int>(rec.vals.size());
    out.insert(out.end(), reinterpret_cast<char *>(&n),
               reinterpret_cast<char *>(&n) + sizeof(int));

    for (const auto &v : rec.vals)
    {
        out.insert(out.end(),
                   reinterpret_cast<const char *>(&v.type),
                   reinterpret_cast<const char *>(&v.type) + sizeof(int));

        if (v.type == TYPE_INT)
        {
            out.insert(out.end(),
                       reinterpret_cast<const char *>(&v.i),
                       reinterpret_cast<const char *>(&v.i) + sizeof(int));
        }
        else
        {
            int len = static_cast<int>(v.s.size());
            out.insert(out.end(),
                       reinterpret_cast<char *>(&len),
                       reinterpret_cast<char *>(&len) + sizeof(int));
            out.insert(out.end(), v.s.begin(), v.s.end());
        }
    }

    return out;
}

Record deserialize(const std::vector<char> &data)
{
    Record rec;
    int offset = 0;

    if (static_cast<int>(data.size()) < static_cast<int>(sizeof(int)))
        return rec;

    int n = 0;
    std::memcpy(&n, &data[offset], sizeof(int));
    offset += sizeof(int);

    // Sanity-check: a corrupt record with a huge n would OOM. Cap at 1000 cols.
    if (n < 0 || n > 1000)
        return rec;

    for (int i = 0; i < n; i++)
    {
        Value v;

        if (offset + static_cast<int>(sizeof(int)) > static_cast<int>(data.size()))
            return rec;
        std::memcpy(&v.type, &data[offset], sizeof(int));
        offset += sizeof(int);

        if (v.type == TYPE_INT)
        {
            if (offset + static_cast<int>(sizeof(int)) > static_cast<int>(data.size()))
                return rec;
            std::memcpy(&v.i, &data[offset], sizeof(int));
            offset += sizeof(int);
        }
        else
        {
            int len = 0;
            if (offset + static_cast<int>(sizeof(int)) > static_cast<int>(data.size()))
                return rec;
            std::memcpy(&len, &data[offset], sizeof(int));
            offset += sizeof(int);

            if (len < 0 || offset + len > static_cast<int>(data.size()))
                return rec;

            v.s = std::string(data.begin() + offset, data.begin() + offset + len);
            offset += len;
        }

        rec.vals.push_back(v);
    }

    return rec;
}