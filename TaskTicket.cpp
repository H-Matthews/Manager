#include "TaskTicket.h"
#include <cassert>
#include "TaskScheduleManager.h"   // needed for manager_->release(...) below

TaskTicket::TaskTicket(WorkingTask* workingTask, SlewingTask* slewingTask, TaskScheduleManager* manager)
    : workingTask_(workingTask)
    , slewingTask_(slewingTask)
    , manager_(manager)
{}

TaskTicket::TaskTicket(TaskTicket&& other) noexcept
    : workingTask_(other.workingTask_)
    , slewingTask_(other.slewingTask_)
    , manager_(other.manager_)
{
    other.workingTask_ = nullptr;
    other.slewingTask_ = nullptr;
    other.manager_ = nullptr;
}

TaskTicket& TaskTicket::operator=(TaskTicket&& other) noexcept {
    if (this != &other) {
        if (workingTask_ && manager_) {
            manager_->release(workingTask_);   // this ticket held a live reservation — drop it before overwriting
        }
        workingTask_ = other.workingTask_;
        slewingTask_ = other.slewingTask_;
        manager_ = other.manager_;
        other.workingTask_ = nullptr;
        other.slewingTask_ = nullptr;
        other.manager_ = nullptr;
    }
    return *this;
}

TaskTicket::~TaskTicket() {
    if (workingTask_ && manager_) {
        manager_->release(workingTask_);
    }
}

const WorkingTask& TaskTicket::workingTask() const {
    assert(workingTask_ && "TaskTicket accessed after completeAndRemove (or after being moved from)");
    return *workingTask_;
}

const SlewingTask* TaskTicket::slewingTask() const {
    assert(workingTask_ && "TaskTicket accessed after completeAndRemove (or after being moved from)");
    return slewingTask_;
}
