#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <memory>
#include <optional>
#include <utility>

#include "TaskTypes.h"
#include "TaskScheduleManager.h"

using Clock = std::chrono::system_clock;

static TimePoint at(int minutesFromEpoch) {
    return Clock::time_point(std::chrono::minutes(minutesFromEpoch));
}

static void check(bool condition, const std::string& label) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << "\n";
}

int main() {
    TaskSchedule schedule;
    TaskScheduleManager manager(schedule);

    std::cout << "--- findEarliestAndReserve: ctpId is not unique, earliest wins ---\n";
    {
        auto* slewA = new SlewingTask(/*id=*/100, /*endTime=*/at(10));
        schedule.addTask(slewA);

        auto* workA = new WorkingTask(/*id=*/1, /*ctpId=*/500, Direction::Sender, at(20), at(30), slewA);
        auto* workB = new WorkingTask(/*id=*/2, /*ctpId=*/500, Direction::Sender, at(25), at(35));
        auto* workC = new WorkingTask(/*id=*/3, /*ctpId=*/500, Direction::Receiver, at(40), at(50));
        schedule.addTask(workA);
        schedule.addTask(workB);
        schedule.addTask(workC);

        auto ticket1 = manager.findEarliestAndReserve(500, Direction::Sender);
        check(ticket1.has_value(), "finds a match");
        check(ticket1->workingTask().getId() == 1, "earliest (workA, start=20) wins over workB (start=25)");
        check(ticket1->slewingTask() != nullptr && ticket1->slewingTask()->getId() == 100, "ticket carries the linked SlewingTask");

        auto ticket2 = manager.findEarliestAndReserve(500, Direction::Sender);
        check(ticket2.has_value() && ticket2->workingTask().getId() == 2, "second lookup finds workB once workA is reserved");

        auto ticket3 = manager.findEarliestAndReserve(500, Direction::Sender);
        check(!ticket3.has_value(), "no unreserved match remains once both workA and workB are checked out");

        auto receiverTicket = manager.findEarliestAndReserve(500, Direction::Receiver);
        check(receiverTicket.has_value() && receiverTicket->workingTask().getId() == 3, "direction filter finds workC for Receiver");

        manager.completeAndRemove(std::move(*ticket1));
        manager.completeAndRemove(std::move(*ticket2));
        manager.completeAndRemove(std::move(*receiverTicket));

        auto afterRemoval = manager.findEarliestAndReserve(500, Direction::Sender);
        check(!afterRemoval.has_value(), "no match remains after everything is completed and removed");
    }

    std::cout << "\n--- findWithinPeriodAndReserve: direction + strictly-exclusive window ---\n";
    {
        auto* periodTaskEarly = new WorkingTask(10, /*ctpId=*/501, Direction::Sender, at(20), at(30));
        auto* periodTaskLate  = new WorkingTask(11, /*ctpId=*/502, Direction::Sender, at(25), at(35));
        schedule.addTask(periodTaskEarly);
        schedule.addTask(periodTaskLate);

        auto ticket = manager.findWithinPeriodAndReserve(Direction::Sender, at(15), at(40));
        check(ticket.has_value(), "finds a match in (15,40)");
        check(ticket->workingTask().getId() == 10, "earliest in the window (start=20) wins over start=25");
        manager.completeAndRemove(std::move(*ticket));

        auto* boundaryTask = new WorkingTask(12, /*ctpId=*/503, Direction::Receiver, at(50), at(60));
        schedule.addTask(boundaryTask);
        auto boundaryMiss = manager.findWithinPeriodAndReserve(Direction::Receiver, at(40), at(50));
        check(!boundaryMiss.has_value(), "a task exactly on the period boundary is excluded");
    }

    std::cout << "\n--- findPrecedingSlew + scheduleAndConsumeTasks ---\n";
    {
        TimePoint newStart = at(70);
        auto preceding = manager.findPrecedingSlew(newStart);
        check(!preceding.has_value(), "no preceding slew found once the earlier one was removed with its WorkingTask");

        std::vector<std::unique_ptr<Task>> pair;
        auto newSlew = std::make_unique<SlewingTask>(200, at(65));
        auto newWork = std::make_unique<WorkingTask>(4, /*ctpId=*/600, Direction::Sender, newStart, at(80), newSlew.get());
        pair.push_back(std::move(newSlew));
        pair.push_back(std::move(newWork));

        bool scheduled = manager.scheduleAndConsumeTasks(pair, newStart, std::nullopt);
        check(scheduled, "succeeds when predecessor assumption matches (none expected, none found)");
        check(pair[0] == nullptr && pair[1] == nullptr, "unique_ptrs released after a successful schedule");

        auto currentPreceding = manager.findPrecedingSlew(at(100));
        check(currentPreceding.has_value() && currentPreceding->id == 200, "the newly-added SlewingTask is now the correct predecessor");

        // A caller that computed its slew assuming *no* predecessor, even
        // though one now exists — the stale assumption must be rejected.
        auto staleSlew = std::make_unique<SlewingTask>(201, at(95));
        auto staleWork = std::make_unique<WorkingTask>(5, /*ctpId=*/601, Direction::Sender, at(100), at(110), staleSlew.get());
        std::vector<std::unique_ptr<Task>> stalePair;
        stalePair.push_back(std::move(staleSlew));
        stalePair.push_back(std::move(staleWork));

        bool staleResult = manager.scheduleAndConsumeTasks(stalePair, at(100), std::nullopt);
        check(!staleResult, "stale predecessor assumption (none) is rejected now that one exists");
        check(stalePair[0] != nullptr && stalePair[1] != nullptr, "unique_ptrs still own their memory after a rejected schedule");

        bool retryResult = manager.scheduleAndConsumeTasks(stalePair, at(100), 200);
        check(retryResult, "retrying with the correct predecessor succeeds");
        check(stalePair[0] == nullptr && stalePair[1] == nullptr, "unique_ptrs released after the successful retry");
    }

    std::cout << "\n--- tryTaskRemoval ---\n";
    {
        auto* toExpire = new WorkingTask(20, /*ctpId=*/800, Direction::Receiver, at(200), at(210));
        schedule.addTask(toExpire);

        auto result = manager.tryTaskRemoval(20);
        check(!result.has_value(), "cleanup successfully removes an unreserved, expired WorkingTask");

        auto again = manager.tryTaskRemoval(20);
        check(again == TaskScheduleManager::RemovalError::UNKNOWN, "a second sweep reports UNKNOWN rather than removing again");
    }

    std::cout << "\n--- tryTaskRemoval skips a currently-reserved task ---\n";
    {
        auto* stillNeeded = new WorkingTask(21, /*ctpId=*/801, Direction::Sender, at(250), at(260));
        schedule.addTask(stillNeeded);

        auto held = manager.findEarliestAndReserve(801, Direction::Sender);
        check(held.has_value(), "task reserved ahead of a cleanup race");

        auto result = manager.tryTaskRemoval(21);   // cleanup runs while it's still checked out
        check(result == TaskScheduleManager::RemovalError::RESERVED, "cleanup reports RESERVED, distinct from UNKNOWN");

        bool stillInSchedule = false;
        for (auto* t : schedule.getTasks<WorkingTask>()) {
            if (t->getId() == 21) stillInSchedule = true;
        }
        check(stillInSchedule, "cleanup skipped the reserved task instead of removing it");

        manager.completeAndRemove(std::move(*held));

        stillInSchedule = false;
        for (auto* t : schedule.getTasks<WorkingTask>()) {
            if (t->getId() == 21) stillInSchedule = true;
        }
        check(!stillInSchedule, "task is actually removed once completeAndRemove runs");
    }

    std::cout << "\n--- concurrency: two threads racing for the same task ---\n";
    {
        auto* contested = new WorkingTask(30, /*ctpId=*/900, Direction::Sender, at(300), at(310));
        schedule.addTask(contested);

        std::atomic<int> winners{0};
        auto attempt = [&]() {
            auto ticket = manager.findEarliestAndReserve(900, Direction::Sender);
            if (ticket.has_value()) {
                winners++;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));   // simulate "processing"
                manager.completeAndRemove(std::move(*ticket));
            }
        };

        std::thread t1(attempt);
        std::thread t2(attempt);
        t1.join();
        t2.join();

        check(winners == 1, "exactly one thread reserved the contested WorkingTask");
    }

    std::cout << "\nDone.\n";
    return 0;
}
