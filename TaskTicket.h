#pragma once
#include "TaskTypes.h"

class TaskScheduleManager;  // forward decl only — see TaskTicket.cpp for why

// A move-only handle on a reserved WorkingTask (and its linked SlewingTask,
// if any). Tasks are immutable once scheduled, and reservation guarantees
// nothing can remove this task while the ticket is alive — so Controller
// reads straight off the live object, const-only, no copying required.
//
// A ticket becomes "spent" once TaskScheduleManager::completeAndRemove has
// consumed it (or once it's been moved from). Accessing a spent ticket is
// a programming error and is asserted, not silently allowed.
class TaskTicket {
public:
    TaskTicket(TaskTicket&& other) noexcept;
    TaskTicket& operator=(TaskTicket&& other) noexcept;
    TaskTicket(const TaskTicket&) = delete;
    TaskTicket& operator=(const TaskTicket&) = delete;

    // Defined in TaskTicket.cpp, once TaskScheduleManager is a complete type.
    ~TaskTicket();

    const WorkingTask& workingTask() const;   // asserts not spent
    const SlewingTask* slewingTask() const;   // may be null; asserts not spent

private:
    friend class TaskScheduleManager;
    TaskTicket(WorkingTask* workingTask, SlewingTask* slewingTask, TaskScheduleManager* manager);

    // Marks the ticket spent: nulls all three fields, so the destructor
    // no-ops and workingTask()/slewingTask() assert if called afterward.
    // Called by TaskScheduleManager::completeAndRemove once it has
    // finished removing the underlying task — the ticket owns what
    // "spent" means, rather than the manager reaching in field by field.
    void spend();

    WorkingTask*          workingTask_;
    SlewingTask*          slewingTask_;
    TaskScheduleManager*  manager_;
};
