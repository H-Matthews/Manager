#pragma once
#include <vector>
#include <chrono>
#include <mutex>
#include <memory>

// ---------------------------------------------------------------------
// Concrete stand-ins for Task/WorkingTask/SlewingTask/TaskSchedule —
// enough to compile and exercise TaskScheduleManager end to end.
// Replace with your real headers; the interface shape is what matters.
// ---------------------------------------------------------------------

using TaskId = int;      // unique across every Task, WorkingTask or SlewingTask alike
using CtpId  = int;      // business correlation id — distinct from TaskId, NOT guaranteed unique
using TimePoint = std::chrono::system_clock::time_point;
enum class Direction { Sender, Receiver };

class Task {
public:
    virtual ~Task() = default;
    virtual TaskId getId() const = 0;
};

class SlewingTask : public Task {
public:
    SlewingTask(TaskId id, TimePoint endTime) : id_(id), endTime_(endTime) {}
    TaskId getId() const override { return id_; }
    TimePoint getEndTime() const { return endTime_; }
    // ... extend with whatever AzEl fields Controller actually reads
private:
    TaskId id_;
    TimePoint endTime_;
};

class WorkingTask : public Task {
public:
    WorkingTask(TaskId id, CtpId ctpId, Direction direction, TimePoint startTime, TimePoint endTime,
                SlewingTask* slewingTask = nullptr)
        : id_(id), ctpId_(ctpId), direction_(direction),
          startTime_(startTime), endTime_(endTime), slewingTask_(slewingTask) {}

    TaskId getId() const override { return id_; }
    CtpId getCtpId() const { return ctpId_; }
    Direction getDirection() const { return direction_; }
    TimePoint getStartTime() const { return startTime_; }
    TimePoint getEndTime() const { return endTime_; }
    SlewingTask* getSlewingTask() const { return slewingTask_; }

private:
    TaskId id_;
    CtpId ctpId_;
    Direction direction_;
    TimePoint startTime_;
    TimePoint endTime_;
    SlewingTask* slewingTask_;
};

class TaskSchedule {
public:
    template <typename T>
    std::vector<T*> getTasks() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T*> result;
        for (auto& t : tasks_) {
            if (auto* casted = dynamic_cast<T*>(t.get())) {
                result.push_back(casted);
            }
        }
        return result;
    }

    bool addTask(Task* task) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : tasks_) {
            if (t->getId() == task->getId()) return false;   // reject duplicate IDs
        }
        tasks_.emplace_back(task);
        return true;
    }

    void removeTask(Task* task) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
            if (it->get() == task) {
                tasks_.erase(it);   // unique_ptr destructor deletes it
                return;
            }
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Task>> tasks_;
};
