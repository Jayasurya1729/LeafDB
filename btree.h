#ifndef BTREE_H
#define BTREE_H

#include <utility>
#include <vector>

int cmp(const std::vector<char> &a, const std::vector<char> &b);

struct Node;

class Bplustree
{
private:
    Node *root;

    Node *findleaf(Node *node, std::vector<char> key);
    void insertIntoParent(Node *left, std::vector<char> key, Node *right);
    void splitInternal(Node *node);
    void splitleaf(Node *leaf);
    void mergeLeaf(Node *left, Node *right, int idx);
    void mergeInternal(Node *left, Node *right, int idx);
    void fixInternal(Node *node);
    void fixLeaf(Node *leaf);

public:
    Bplustree();
    ~Bplustree();

    void clear();
    void insert(std::vector<char> key, std::vector<char> value);
    std::vector<char> search(std::vector<char> key);
    void remove(std::vector<char> key);
    Node *findLeafPublic(std::vector<char> key);
    bool startsWith(const std::vector<char> &key, const std::vector<char> &prefix);
    std::vector<std::vector<char>> scanPrefix(const std::vector<char> &prefix);
    std::vector<std::pair<std::vector<char>, std::vector<char>>> scanPrefixKV(const std::vector<char> &prefix);
    std::vector<std::pair<std::vector<char>, std::vector<char>>> scanRange(
        const std::vector<char> &start,
        const std::vector<char> &end);
};

#endif
