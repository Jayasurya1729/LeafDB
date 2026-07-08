#include "lock_manager.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace
{
    bool isTableResource(const std::string &resourceId)
    {
        return resourceId.find(":row:") == std::string::npos;
    }
}

bool LockManager::canGrant(const LockEntry &entry, int transactionId, LockMode mode) const
{
    for (const auto &holder : entry.holders)
    {
        if (holder.first == transactionId)
            continue;
        if (!areCompatible(holder.second, mode))
            return false;
    }
    return true;
}

void LockManager::grant(LockEntry &entry, int transactionId, LockMode mode)
{
    entry.holders[transactionId] = mode;
}

void LockManager::releaseResourceFromEntry(LockEntry &entry, int transactionId)
{
    entry.holders.erase(transactionId);
}

bool LockManager::acquireLockInternal(int transactionId,
                                      const std::string &resourceId,
                                      LockMode mode,
                                      const std::chrono::steady_clock::time_point &deadline,
                                      std::vector<std::string> &acquiredResources,
                                      std::unique_lock<std::mutex> &lock)
{
    while (true)
    {
        auto &entry = lockTable_[resourceId];
        if (canGrant(entry, transactionId, mode))
        {
            grant(entry, transactionId, mode);
            acquiredResources.push_back(resourceId);
            transactionResources_[transactionId].insert(resourceId);
            return true;
        }

        if (std::chrono::steady_clock::now() >= deadline)
            return false;

        condition_.wait_until(lock, deadline);   // <-- pass the lock, not mutex_
    }
}

std::string LockManager::parentResourceFor(const std::string &resourceId) const
{
    if (resourceId.find(":row:") == std::string::npos)
        return {};

    size_t tableStart = resourceId.find("table:");
    if (tableStart == std::string::npos)
        return {};

    size_t rowPos = resourceId.find(":row:");
    if (rowPos == std::string::npos)
        return {};

    return resourceId.substr(0, rowPos);
}

LockManager::LockMode LockManager::parentModeFor(LockMode mode) const
{
    switch (mode)
    {
    case LockMode::Shared:
    case LockMode::IntentionShared:
        return LockMode::IntentionShared;
    case LockMode::Exclusive:
    case LockMode::IntentionExclusive:
        return LockMode::IntentionExclusive;
    }
    return LockMode::IntentionShared;
}

bool LockManager::areCompatible(LockMode left, LockMode right) const
{
    if (left == LockMode::Exclusive || right == LockMode::Exclusive)
        return false;

    if ((left == LockMode::IntentionExclusive && right == LockMode::Shared) ||
        (left == LockMode::Shared && right == LockMode::IntentionExclusive))
        return false;

    return true; // IS-IS, IS-IX, IS-S, IX-IX, S-S all compatible
}
void LockManager::rollbackAcquiredLocks(int transactionId,
                                         const std::vector<std::string> &resources)
{
    for (const auto &resourceId : resources)
    {
        auto lockIt = lockTable_.find(resourceId);
        if (lockIt == lockTable_.end())
            continue;
        releaseResourceFromEntry(lockIt->second, transactionId);
        if (lockIt->second.holders.empty())
            lockTable_.erase(lockIt);
    }
    transactionResources_.erase(transactionId);
}

bool LockManager::acquireLock(int transactionId,
                              const std::string &resourceId,
                              LockMode mode,
                              std::chrono::milliseconds timeout)
{
    if (transactionId <= 0)
        throw std::runtime_error("Invalid transaction id");

    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::string> acquiredResources;

    if (resourceId.find(":row:") != std::string::npos)
    {
        std::string parent = parentResourceFor(resourceId);
        if (!parent.empty())
        {
            if (!acquireLockInternal(transactionId, parent, parentModeFor(mode), deadline, acquiredResources, lock))
            {
                rollbackAcquiredLocks(transactionId, acquiredResources);
                return false;
            }
        }
    }

    if (!acquireLockInternal(transactionId, resourceId, mode, deadline, acquiredResources, lock))
    {
        rollbackAcquiredLocks(transactionId, acquiredResources);
        return false;
    }

    return true;
}

void LockManager::releaseAllLocks(int transactionId)
{
    if (transactionId <= 0)
        return;

    std::unique_lock<std::mutex> lock(mutex_);
    auto txIt = transactionResources_.find(transactionId);
    if (txIt == transactionResources_.end())
        return;

    for (const auto &resourceId : txIt->second)
    {
        auto lockIt = lockTable_.find(resourceId);
        if (lockIt == lockTable_.end())
            continue;
        releaseResourceFromEntry(lockIt->second, transactionId);
        if (lockIt->second.holders.empty())
            lockTable_.erase(lockIt);
    }

    transactionResources_.erase(txIt);
    condition_.notify_all();
}
