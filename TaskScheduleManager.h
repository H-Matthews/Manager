#pragma once
#include <mutex>
#include <unordered_set>
#include <functional>
#include <optional>
#include <variant>
#include <vector>
#include <memory>
#include "TaskTypes.h"
#include "TaskTicket.h"

// Sole access point for WorkingTask/SlewingTask lookups, creation, and
// removal. Nothing outside this class should ever call
// TaskSchedule::addTask/removeTask or dereference a Task* obtained from
// it directly.
class TaskScheduleManager {
public:
    explicit TaskScheduleManager(TaskSchedule& schedule) : schedule_(schedule) {}

    using Predicate = std::function<bool(const WorkingTask&)>;
    using IsBetter  = std::function<bool(const WorkingTask&, const WorkingTask&)>; // true if 'a' beats 'b'

    // Shared core behind the named lookups below: scans, filters by
    // 'matches', picks the winner by 'isBetter', reserves it (and its
    // linked SlewingTask, if any) — all in one locked pass.
    std::optional<TaskTicket> findBestAndReserve(const Predicate& matches, const IsBetter& isBetter);

    // ctpId is NOT guaranteed unique — among matches, the earliest wins.
    std::optional<TaskTicket> findEarliestAndReserve(CtpId ctpId, Direction direction);

    // Direction match + strictly inside (periodStart, periodEnd) — boundary
    // values are excluded. Among matches, the earliest wins.
    std::optional<TaskTicket> findWithinPeriodAndReserve(Direction direction, TimePoint periodStart, TimePoint periodEnd);

    // Controller calls this once processing finishes successfully.
    // Removes the WorkingTask and its linked SlewingTask (if any).
    // Takes the ticket by value — forces the caller to std::move it in,
    // and leaves their local ticket "spent" (further access on it asserts)
    // rather than silently stale if they try to reuse it afterward.
    void completeAndRemove(TaskTicket ticket);

    // Why findAndReserve couldn't produce a ticket for this ID. UNKNOWN
    // and RESERVED both mean "no ticket," but for different reasons —
    // callers (the cleanup thread, mainly) that want to log/alert on a
    // stuck reservation need to tell them apart rather than treating
    // both as one failure case.
    enum class ReserveError {
        UNKNOWN,   // no task with this ID — already removed some other way
        RESERVED   // found, but currently reserved by someone else
    };

    // Looks up a specific WorkingTask by ID and reserves it (and its
    // linked SlewingTask, if any) if found and not already reserved.
    // Used by the periodic cleanup thread: grab the task, examine its
    // current fields (e.g. confirm it's actually still expired), then
    // either completeAndRemove it, or just let the returned ticket fall
    // out of scope to put it back for the next sweep.
    std::variant<TaskTicket, ReserveError> findAndReserve(TaskId workingTaskId);

    // --- Task creation: predecessor lookup + atomic add ---

    struct PrecedingSlewInfo {
        TaskId    id;
        TimePoint endTime;
        // ... extend with whatever AzEl fields the calculation needs
    };

    // Read-only: finds the SlewingTask whose end time is latest among
    // those still strictly before newWorkingTaskStartTime. No reservation
    // involved — this is a snapshot read, safe under the lock regardless
    // of whether the result happens to be reserved by someone else.
    std::optional<PrecedingSlewInfo> findPrecedingSlew(TimePoint newWorkingTaskStartTime) const;

    // Re-validates that 'expectedPrecedingId' is still the correct
    // predecessor for newWorkingTaskStartTime, and if so, adds every task
    // in 'tasks' — all or nothing. On success, ownership of each task
    // moves to TaskSchedule (each unique_ptr is released right at the
    // point its addTask call succeeds). On failure — stale predecessor,
    // or TaskSchedule rejected an add partway through — anything already
    // added in this call is rolled back via removeTask, and every
    // remaining unique_ptr in 'tasks' is left exactly as it was: still
    // owning its memory, nothing for the caller to clean up by hand.
    bool scheduleAndConsumeTasks(std::vector<std::unique_ptr<Task>>& tasks,
                                  TimePoint newWorkingTaskStartTime,
                                  std::optional<TaskId> expectedPrecedingId);

    // Rolls back a WorkingTask (and its linked SlewingTask, if any) that
    // scheduleAndConsumeTasks already committed earlier in the same
    // session, when a later step in that session fails. No reservation
    // check — safe only because session build-and-rollback is fast and
    // synchronous enough that Controller can't realistically have
    // reserved it in the interim (documented assumption, not enforced).
    void unscheduleWorkingTask(WorkingTask* workingTask);

private:
    friend class TaskTicket;

    // Drops a reservation without removing anything — used when a ticket
    // is destroyed (or overwritten via move-assignment) without
    // completeAndRemove having been called.
    void release(WorkingTask* workingTask);

    // Removes a WorkingTask and its linked SlewingTask (if any), and
    // clears both reservations. Caller must already hold mutex_.
    void removeCascadeLocked(WorkingTask* workingTask);

    // Caller must already hold mutex_.
    std::optional<PrecedingSlewInfo> findPrecedingSlewLocked(TimePoint newWorkingTaskStartTime) const;

    static bool startsEarlier(const WorkingTask& a, const WorkingTask& b);

    TaskSchedule& schedule_;
    mutable std::mutex mutex_;

    // One set: WorkingTask and SlewingTask IDs share a single pool, so
    // there's no ambiguity from keeping them in the same set.
    std::unordered_set<TaskId> reserved_;
};
