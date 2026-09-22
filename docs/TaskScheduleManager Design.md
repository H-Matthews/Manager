# TaskScheduleManager Design — Cooperative Per-ID Reservation

Sep 21, 2026 · @Someone

## Overview

`TaskScheduleManager` wraps `TaskSchedule` (which just stores raw `Task*`, no thread safety) so multiple `Controller` threads and a periodic cleanup thread can safely pull tasks off it at the same time.

The hard part isn't the lookup itself — it's that `Controller`'s real work (external correlation) is slow, and we don't want to hold a lock for the whole thing. So the design's goal is: lock briefly to claim a task, do the slow work unlocked, and never let two threads touch the same task at once — without copying data out just to be safe.

## The pattern: cooperative per-ID reservation

We call this **cooperative per-ID reservation**. It's a specific flavor of a couple of well-known ideas:

- **Advisory locking.** "Reserved" isn't enforced by the runtime — it's just a set of IDs (`reserved_`) that every access path *chooses* to check. Nothing stops new code from grabbing a `Task*` directly and ignoring the set. Same idea as `flock()`: it only blocks callers that also call `flock()`.
- **Reservation / checkout.** Briefly lock a shared structure to mark one item as exclusively yours, then do the real (slow) work unlocked, then explicitly release or complete. Same shape as a DB `SELECT ... FOR UPDATE` on one row, or a message queue's visibility timeout / lease (checked out, hidden from others, explicitly deleted or it times back to visible).

Worth being precise about what it's *not*: this isn't optimistic concurrency control. OCC lets everyone proceed and detects conflicts after the fact (versions, compare-and-swap, retry). This is pessimistic by intent — the reservation is taken *before* anyone reads, specifically so the conflict never happens — just implemented cheaply (one shared ID set) instead of a lock object per task.

## How it works

One `mutex_` plus one `reserved_` set of `TaskId` live on `TaskScheduleManager`. WorkingTask and SlewingTask IDs share the same pool — no ambiguity from keeping them separate.

"Reserved" means exactly two things:

1. No one else can pick this task in a find call.
2. No one can remove it.

That's the entire safety story, and it rests on one invariant: **tasks are immutable once scheduled.** Created once (under the lock, in `scheduleAndConsumeTasks`), and from then on only ever *removed* — never edited in place. Because nothing writes to a live task's fields, and reservation blocks the one thing that could invalidate a pointer (removal), `Controller` can read straight off the live object during its unlocked processing with zero risk of a race.

If that invariant ever stops being true — something starts mutating a scheduled task's fields in place — this whole design needs revisiting.

## The ticket

`TaskTicket` is the RAII handle a find call returns. Early on it copied out the fields `Controller` needed into a snapshot — once we confirmed tasks are immutable while reserved, that copy turned out to be unnecessary work protecting against a race that couldn't happen. So it was simplified to just hold pointers:

- `const WorkingTask& workingTask()` and `const SlewingTask* slewingTask()` — const-only, so the type system enforces "nothing mutates a reserved task," not just a comment.
- Both assert if called on a **spent** ticket (one that's already been completed, or moved from).
- Move-only. The move ctor/assignment explicitly null out the source's pointers — the compiler's default move for raw pointers just copies the bits, which would've left both the moved-from and moved-to ticket holding the same reservation and both trying to release it.
- A private `spend()` method nulls all three fields at once. `TaskScheduleManager::completeAndRemove` calls it rather than reaching into the ticket's fields by hand — the ticket owns what "spent" means, not its caller.
- If a ticket is ever dropped without completing (crash, bug), the destructor just releases the reservation. Treated as a caller bug, not something the framework tries to force-evict.

## Completing and cleanup

**`completeAndRemove(TaskTicket ticket)`** takes the ticket **by value**, which forces the caller to `std::move` it in. That leaves their local variable moved-from (spent) if they try to touch it again — a compile-time-shaped nudge instead of a silent bug. Internally: lock, remove the `WorkingTask` and its linked `SlewingTask` (if any) via `removeCascadeLocked`, spend the ticket, unlock.

`findAndReserve(TaskId)` is what the periodic cleanup thread calls to grab a specific, already-identified task by ID and examine it before deciding anything — the direct-lookup counterpart to the scanning finds above. Returns `std::variant<TaskTicket, ReserveError>`:

- `TaskTicket` — reserved successfully. Examine `ticket.workingTask()`, then either `completeAndRemove` it, or just let it fall out of scope to put it back for the next sweep.
- `ReserveError::UNKNOWN` — no task with this ID (already removed some other way)
- `ReserveError::RESERVED` — found, but currently held by someone else

Splitting those two failure reasons still matters the way it did for the old `RemovalError`: `UNKNOWN` is the common, expected case and can log at debug level; `RESERVED` recurring for the *same* ID across sweeps is the interesting one — a signal a ticket may be stuck.

Unlike `findAndReserve`, the scanning finds (`findBestAndReserve` and friends) still return a plain `std::optional<TaskTicket>`. A failed scan there is an aggregate outcome over many candidates, each of which could have failed for a different reason — there's no single honest cause to surface the way there is for a lookup that targets one specific, already-identified task.

## Interface at a glance

```cpp
std::optional<TaskTicket> findBestAndReserve(const Predicate& matches, const IsBetter& isBetter);
std::optional<TaskTicket> findEarliestAndReserve(CtpId ctpId, Direction direction);
std::optional<TaskTicket> findWithinPeriodAndReserve(Direction direction, TimePoint periodStart, TimePoint periodEnd);

void completeAndRemove(TaskTicket ticket);   // by value — forces std::move at the call site

enum class ReserveError { UNKNOWN, RESERVED };
std::variant<TaskTicket, ReserveError> findAndReserve(TaskId workingTaskId);

bool scheduleAndConsumeTasks(std::vector<std::unique_ptr<Task>>& tasks,
                              TimePoint newWorkingTaskStartTime,
                              std::optional<TaskId> expectedPrecedingId);
```

```cpp
class TaskTicket {
public:
    TaskTicket(TaskTicket&&) noexcept;
    TaskTicket& operator=(TaskTicket&&) noexcept;
    TaskTicket(const TaskTicket&) = delete;
    TaskTicket& operator=(const TaskTicket&) = delete;
    ~TaskTicket();   // releases reservation if not already spent

    const WorkingTask& workingTask() const;   // asserts not spent
    const SlewingTask* slewingTask() const;   // may be null; asserts not spent
};
```

## Gotchas

**Use-after-free in `removeCascadeLocked`, found by AddressSanitizer.** The original code called `schedule_.removeTask(s)` (which deletes the object) and then, on the next line, read `s->getId()` on that same now-freed pointer to erase it from `reserved_`. Fix: capture each ID *before* calling `removeTask`, not after. Only triggered when a `WorkingTask` actually had a linked `SlewingTask` — the kind of bug that stays quiet for a long time and then shows up as a flaky crash under load.

**Reading a ticket after `completeAndRemove` is a live use-after-free, not just stale data.** This is stricter than the original copied-snapshot design: previously, holding onto a ticket past completion was harmless (the copy just sat there). Now that the ticket wraps a pointer, the object behind it is genuinely gone. The `assert` on spent access turns that into a loud dev-time failure instead of silent corruption — but it's worth remembering the tradeoff was made deliberately, not an oversight.
