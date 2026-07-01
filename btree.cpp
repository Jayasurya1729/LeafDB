#include "btree.h"
#include <algorithm>

const int order = 4;

// Unsigned byte comparison so keys with high-byte values sort correctly.
int cmp(const std::vector<char> &a, const std::vector<char> &b)
{
    int n = std::min(static_cast<int>(a.size()), static_cast<int>(b.size()));
    for (int i = 0; i < n; i++)
    {
        unsigned char ua = static_cast<unsigned char>(a[i]);
        unsigned char ub = static_cast<unsigned char>(b[i]);
        if (ua < ub) return -1;
        if (ua > ub) return  1;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return  1;
    return 0;
}

struct Node
{
    bool isleaf;
    std::vector<std::vector<char>> keys;
    std::vector<std::vector<char>> values;
    std::vector<Node *> child;
    Node *next;
    Node *parent;

    explicit Node(bool leaf) : isleaf(leaf), next(nullptr), parent(nullptr) {}
};

namespace
{
    void deleteSubtree(Node *node)
    {
        if (!node) return;
        if (!node->isleaf)
            for (Node *c : node->child)
                deleteSubtree(c);
        delete node;
    }
}

Bplustree::Bplustree()  { root = new Node(true); }
Bplustree::~Bplustree() { deleteSubtree(root); }

void Bplustree::clear()
{
    deleteSubtree(root);
    root = new Node(true);
}

Node *Bplustree::findleaf(Node *node, std::vector<char> key)
{
    if (node->isleaf) return node;
    for (int i = 0; i < static_cast<int>(node->keys.size()); i++)
        if (cmp(key, node->keys[i]) < 0)
            return findleaf(node->child[i], key);
    return findleaf(node->child.back(), key);
}

void Bplustree::insertIntoParent(Node *left, std::vector<char> key, Node *right)
{
    Node *parent = left->parent;
    if (!parent)
    {
        Node *newRoot = new Node(false);
        newRoot->keys.push_back(key);
        newRoot->child.push_back(left);
        newRoot->child.push_back(right);
        left->parent = right->parent = newRoot;
        root = newRoot;
        return;
    }

    int idx = 0;
    while (parent->child[idx] != left) idx++;
    parent->keys.insert(parent->keys.begin() + idx, key);
    parent->child.insert(parent->child.begin() + idx + 1, right);
    right->parent = parent;

    if (static_cast<int>(parent->keys.size()) > order)
        splitInternal(parent);
}

void Bplustree::splitInternal(Node *node)
{
    int mid = static_cast<int>(node->keys.size()) / 2;
    std::vector<char> upKey = node->keys[mid];

    Node *newNode = new Node(false);
    for (int i = mid + 1; i < static_cast<int>(node->keys.size()); i++)
        newNode->keys.push_back(node->keys[i]);
    for (int i = mid + 1; i < static_cast<int>(node->child.size()); i++)
    {
        newNode->child.push_back(node->child[i]);
        node->child[i]->parent = newNode;
    }
    node->keys.resize(mid);
    node->child.resize(mid + 1);
    newNode->parent = node->parent;
    insertIntoParent(node, upKey, newNode);
}

void Bplustree::splitleaf(Node *leaf)
{
    int mid = static_cast<int>(leaf->keys.size()) / 2;
    Node *newLeaf = new Node(true);
    for (int i = mid; i < static_cast<int>(leaf->keys.size()); i++)
    {
        newLeaf->keys.push_back(leaf->keys[i]);
        newLeaf->values.push_back(leaf->values[i]);
    }
    leaf->keys.resize(mid);
    leaf->values.resize(mid);
    newLeaf->next = leaf->next;
    leaf->next = newLeaf;
    newLeaf->parent = leaf->parent;
    insertIntoParent(leaf, newLeaf->keys[0], newLeaf);
}

void Bplustree::mergeLeaf(Node *left, Node *right, int idx)
{
    for (int i = 0; i < static_cast<int>(right->keys.size()); i++)
    {
        left->keys.push_back(right->keys[i]);
        left->values.push_back(right->values[i]);
    }
    left->next = right->next;
    Node *parent = left->parent;
    parent->keys.erase(parent->keys.begin() + idx);
    parent->child.erase(parent->child.begin() + idx + 1);
    delete right;
    fixInternal(parent);
}

void Bplustree::mergeInternal(Node *left, Node *right, int idx)
{
    Node *parent = left->parent;
    left->keys.push_back(parent->keys[idx]);
    for (auto &k : right->keys)  left->keys.push_back(k);
    for (auto  c : right->child) { left->child.push_back(c); c->parent = left; }
    parent->keys.erase(parent->keys.begin() + idx);
    parent->child.erase(parent->child.begin() + idx + 1);
    delete right;
    fixInternal(parent);
}

void Bplustree::fixInternal(Node *node)
{
    if (node == root)
    {
        if (node->keys.empty() && !node->child.empty())
        {
            root = node->child[0];
            root->parent = nullptr;
            delete node;
        }
        return;
    }

    int minKeys = (order + 1) / 2 - 1;
    if (static_cast<int>(node->keys.size()) >= minKeys) return;

    Node *parent = node->parent;
    int idx = 0;
    while (parent->child[idx] != node) idx++;

    Node *left  = (idx > 0) ? parent->child[idx - 1] : nullptr;
    Node *right = (idx < static_cast<int>(parent->child.size()) - 1)
                  ? parent->child[idx + 1] : nullptr;

    if (left && static_cast<int>(left->keys.size()) > minKeys)
    {
        node->keys.insert(node->keys.begin(), parent->keys[idx - 1]);
        parent->keys[idx - 1] = left->keys.back();
        node->child.insert(node->child.begin(), left->child.back());
        left->child.back()->parent = node;
        left->keys.pop_back();
        left->child.pop_back();
        return;
    }

    if (right && static_cast<int>(right->keys.size()) > minKeys)
    {
        node->keys.push_back(parent->keys[idx]);
        parent->keys[idx] = right->keys[0];
        node->child.push_back(right->child[0]);
        right->child[0]->parent = node;
        right->keys.erase(right->keys.begin());
        right->child.erase(right->child.begin());
        return;
    }

    if (left)  mergeInternal(left, node, idx - 1);
    else if (right) mergeInternal(node, right, idx);
}

void Bplustree::fixLeaf(Node *leaf)
{
    if (leaf == root) return;

    int minKeys = (order + 1) / 2;
    if (static_cast<int>(leaf->keys.size()) >= minKeys) return;

    Node *parent = leaf->parent;
    int idx = 0;
    while (parent->child[idx] != leaf) idx++;

    Node *left  = (idx > 0) ? parent->child[idx - 1] : nullptr;
    Node *right = (idx < static_cast<int>(parent->child.size()) - 1)
                  ? parent->child[idx + 1] : nullptr;

    if (left && static_cast<int>(left->keys.size()) > minKeys)
    {
        leaf->keys.insert(leaf->keys.begin(), left->keys.back());
        leaf->values.insert(leaf->values.begin(), left->values.back());
        left->keys.pop_back();
        left->values.pop_back();
        parent->keys[idx - 1] = leaf->keys[0];
        return;
    }

    if (right && static_cast<int>(right->keys.size()) > minKeys)
    {
        leaf->keys.push_back(right->keys[0]);
        leaf->values.push_back(right->values[0]);
        right->keys.erase(right->keys.begin());
        right->values.erase(right->values.begin());
        parent->keys[idx] = right->keys[0];
        return;
    }

    if (left)       mergeLeaf(left, leaf, idx - 1);
    else if (right) mergeLeaf(leaf, right, idx);
}

void Bplustree::insert(std::vector<char> key, std::vector<char> value)
{
    Node *leaf = findleaf(root, key);
    int i = 0;
    while (i < static_cast<int>(leaf->keys.size()) && cmp(leaf->keys[i], key) < 0)
        i++;

    if (i < static_cast<int>(leaf->keys.size()) && cmp(leaf->keys[i], key) == 0)
    {
        leaf->values[i] = value;   // update existing
        return;
    }

    leaf->keys.insert(leaf->keys.begin() + i, key);
    leaf->values.insert(leaf->values.begin() + i, value);

    if (static_cast<int>(leaf->keys.size()) > order)
        splitleaf(leaf);
}

std::vector<char> Bplustree::search(std::vector<char> key)
{
    Node *leaf = findleaf(root, key);
    for (int i = 0; i < static_cast<int>(leaf->keys.size()); i++)
        if (cmp(leaf->keys[i], key) == 0)
            return leaf->values[i];
    return {};
}

void Bplustree::remove(std::vector<char> key)
{
    Node *leaf = findleaf(root, key);
    int idx = -1;
    for (int i = 0; i < static_cast<int>(leaf->keys.size()); i++)
        if (cmp(leaf->keys[i], key) == 0) { idx = i; break; }

    if (idx == -1) return;

    leaf->keys.erase(leaf->keys.begin() + idx);
    leaf->values.erase(leaf->values.begin() + idx);
    fixLeaf(leaf);
}

Node *Bplustree::findLeafPublic(std::vector<char> key)
{
    return findleaf(root, key);
}

bool Bplustree::startsWith(const std::vector<char> &key, const std::vector<char> &prefix)
{
    if (prefix.size() > key.size()) return false;
    for (int i = 0; i < static_cast<int>(prefix.size()); i++)
        if (key[i] != prefix[i]) return false;
    return true;
}

std::vector<std::vector<char>> Bplustree::scanPrefix(const std::vector<char> &prefix)
{
    std::vector<std::vector<char>> result;
    Node *leaf = findLeafPublic(prefix);

    while (leaf)
    {
        for (int i = 0; i < static_cast<int>(leaf->keys.size()); i++)
        {
            if (startsWith(leaf->keys[i], prefix))
                result.push_back(leaf->keys[i]);
            else if (cmp(leaf->keys[i], prefix) > 0)
                return result;
        }
        leaf = leaf->next;
    }
    return result;
}

std::vector<std::pair<std::vector<char>, std::vector<char>>>
Bplustree::scanPrefixKV(const std::vector<char> &prefix)
{
    std::vector<std::pair<std::vector<char>, std::vector<char>>> result;
    Node *leaf = findLeafPublic(prefix);

    while (leaf)
    {
        for (int i = 0; i < static_cast<int>(leaf->keys.size()); i++)
        {
            if (startsWith(leaf->keys[i], prefix))
                result.push_back({leaf->keys[i], leaf->values[i]});
            else if (cmp(leaf->keys[i], prefix) > 0)
                return result;
        }
        leaf = leaf->next;
    }
    return result;
}

std::vector<std::pair<std::vector<char>, std::vector<char>>>
Bplustree::scanRange(const std::vector<char> &start, const std::vector<char> &end)
{
    std::vector<std::pair<std::vector<char>, std::vector<char>>> result;
    Node *leaf = findLeafPublic(start);

    while (leaf)
    {
        for (int i = 0; i < static_cast<int>(leaf->keys.size()); i++)
        {
            if (cmp(leaf->keys[i], start) < 0) continue;
            if (cmp(leaf->keys[i], end)   > 0) return result;
            result.push_back({leaf->keys[i], leaf->values[i]});
        }
        leaf = leaf->next;
    }
    return result;
}