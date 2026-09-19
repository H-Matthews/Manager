#include "TaskScheduleManager.h"

bool TaskScheduleManager::startsEarlier(const WorkingTask& a, const WorkingTask& b) {
    return a.getStartTime() < b.getStartTime();
}

std::optional<TaskTicket> TaskScheduleManager::findBestAndReserve(const Predicate& matches, const IsBetter& isBetter) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<WorkingTask*> tasks = schedule_.getTasks<WorkingTask>();
    WorkingTask* best = nullptr;

    for (WorkingTask* t : tasks) {
        if (reserved_.count(t->getId())) continue;   // already checked out
        if (!matches(*t)) continue;
        if (!best || isBetter(*t, *best)) best = t;
    }

    if (!best) return std::nullopt;

    reserved_.insert(best->getId());
    SlewingTask* slew = best->getSlewingTask();
    if (slew) {
        reserved_.insert(slew->getId());
    }

    return TaskTicket(best, slew, this);
}

std::optional<TaskTicket> TaskScheduleManager::findEarliestAndReserve(CtpId ctpId, Direction direction) {
    return findBestAndReserve(
        [ctpId, direction](const WorkingTask& t) {
            return t.getCtpId() == ctpId && t.getDirection() == direction;
        },
        &TaskScheduleManager::startsEarlier
    );
}

std::optional<TaskTicket> TaskScheduleManager::findWithinPeriodAndReserve(Direction direction, TimePoint periodStart, TimePoint periodEnd) {
    return findBestAndReserve(
        [direction, periodStart, periodEnd](const WorkingTask& t) {
            return t.getDirection() == direction &&
                   t.getStartTime() > periodStart &&
                   t.getStartTime() < periodEnd;      // boundaries excluded
        },
        &TaskScheduleManager::startsEarlier
    );
}

void TaskScheduleManager::completeAndRemove(TaskTicket ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    removeCascadeLocked(ticket.workingTask_);
    ticket.workingTask_ = nullptr;
    ticket.slewingTask_ = nullptr;
    ticket.manager_ = nullptr;
    // ticket is now spent — its destructor (about to run, since it's a
    // by-value parameter) sees null and no-ops instead of releasing a
    // reservation that no longer exists.
}

std::optional<TaskScheduleManager::RemovalError> TaskScheduleManager::tryTaskRemoval(TaskId workingTaskId) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (reserved_.count(workingTaskId)) return RemovalError::RESERVED;   // in use — skip this round

    std::vector<WorkingTask*> tasks = schedule_.getTasks<WorkingTask>();
    for (WorkingTask* t : tasks) {
        if (t->getId() == workingTaskId) {
            removeCascadeLocked(t);
            return std::nullopt;
        }
    }
    return RemovalError::UNKNOWN;   // already removed through the normal path
}

void TaskScheduleManager::release(WorkingTask* workingTask) {
    std::lock_guard<std::mutex> lock(mutex_);
    reserved_.erase(workingTask->getId());
    if (SlewingTask* s = workingTask->getSlewingTask()) {
        reserved_.erase(s->getId());
    }
}

void TaskScheduleManager::removeCascadeLocked(WorkingTask* workingTask) {
    if (SlewingTask* s = workingTask->getSlewingTask()) {
        TaskId slewId = s->getId();   // capture before removeTask deletes *s
        schedule_.removeTask(s);
        reserved_.erase(slewId);
    }
    TaskId workingId = workingTask->getId();   // capture before removeTask deletes *workingTask
    schedule_.removeTask(workingTask);
    reserved_.erase(workingId);
}

std::optional<TaskScheduleManager::PrecedingSlewInfo>
TaskScheduleManager::findPrecedingSlewLocked(TimePoint newWorkingTaskStartTime) const {
    std::vector<SlewingTask*> slews = schedule_.getTasks<SlewingTask>();
    SlewingTask* best = nullptr;

    for (SlewingTask* s : slews) {
        if (s->getEndTime() >= newWorkingTaskStartTime) continue;      // must end strictly before
        if (!best || s->getEndTime() > best->getEndTime()) best = s;   // latest-before wins
    }

    if (!best) return std::nullopt;
    return PrecedingSlewInfo{ best->getId(), best->getEndTime() };
}

std::optional<TaskScheduleManager::PrecedingSlewInfo>
TaskScheduleManager::findPrecedingSlew(TimePoint newWorkingTaskStartTime) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return findPrecedingSlewLocked(newWorkingTaskStartTime);
}

bool TaskScheduleManager::scheduleAndConsumeTasks(std::vector<std::unique_ptr<Task>>& tasks,
                                                   TimePoint newWorkingTaskStartTime,
                                                   std::optional<TaskId> expectedPrecedingId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto preceding = findPrecedingSlewLocked(newWorkingTaskStartTime);
    std::optional<TaskId> precedingId = preceding ? std::optional<TaskId>(preceding->id) : std::nullopt;
    if (precedingId != expectedPrecedingId) {
        return false;   // predecessor changed since it was looked up — caller must retry
    }

    std::vector<Task*> added;
    added.reserve(tasks.size());

    for (auto& taskPtr : tasks) {
        Task* raw = taskPtr.get();
        if (!schedule_.addTask(raw)) {
            for (Task* committed : added) {
                schedule_.removeTask(committed);   // undo everything this call already added
            }
            return false;
        }
        taskPtr.release();   // TaskSchedule owns it now — release right at the point of success
        added.push_back(raw);
    }

    return true;
}

void TaskScheduleManager::unscheduleWorkingTask(WorkingTask* workingTask) {
    std::lock_guard<std::mutex> lock(mutex_);
    removeCascadeLocked(workingTask);
}
