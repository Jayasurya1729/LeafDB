#ifndef LOCK_MANAGER_H
#define LOCK_MANAGER_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class LockManager
{
public:
    enum class LockMode
    {
        Shared,
        Exclusive,
        IntentionShared,
        IntentionExclusive
    };

    bool acquireLock(int transactionId,
                     const std::string &resourceId,
                     LockMode mode,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    void releaseAllLocks(int transactionId);

private:
    struct LockEntry
    {
        std::unordered_map<int, LockMode> holders;
    };

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::string, LockEntry> lockTable_;
    std::unordered_map<int, std::set<std::string>> transactionResources_;

    bool canGrant(const LockEntry &entry, int transactionId, LockMode mode) const;
    void grant(LockEntry &entry, int transactionId, LockMode mode);
    void releaseResourceFromEntry(LockEntry &entry, int transactionId);
    bool acquireLockInternal(int transactionId,
                         const std::string &resourceId,
                         LockMode mode,
                         const std::chrono::steady_clock::time_point &deadline,
                         std::vector<std::string> &acquiredResources,
                         std::unique_lock<std::mutex> &lock);
    std::string parentResourceFor(const std::string &resourceId) const;
    LockMode parentModeFor(LockMode mode) const;
    bool areCompatible(LockMode left, LockMode right) const;
    void rollbackAcquiredLocks(int transactionId,
                               const std::vector<std::string> &resources);
};

#endif
