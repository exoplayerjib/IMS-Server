#ifndef ACTOR_THREAD_POOL_H
#define ACTOR_THREAD_POOL_H

#include <vector>
#include <queue>
#include "executor.h"
#include <shared_mutex>
#include <unordered_map>
#include <functional>
#include "eventhandler.h"
#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>

typedef std::function<void()> IO_Task;

/**
 * @brief ActorThreadPool
 *
 * A cooperative actor-based thread pool that ensures tasks for a single
 * actor (IEventHandler*) never execute concurrently. Each actor has a FIFO
 * queue of pending IO_Task objects; the pool shares a single underlying
 * Executor that provides worker threads which actually run the tasks.
 *
 * Concurrency model:
 * - Submitting a task for an actor that is not currently executing will
 *   schedule the task immediately on the Executor and mark the actor as
 *   active/ready.
 * - Submitting a task for an actor that is already executing will enqueue
 *   the task on the actor's per-actor queue. When the running task completes
 *   the next queued task is scheduled.
 */
class ActorThreadPool {
    private:
        Executor threadpool;
        std::unordered_map<IEventHandler*, std::queue<IO_Task>> actor_tasks;
        std::shared_mutex actor_tasks_mutex; /**< Guards actor_tasks. */
        std::vector<IEventHandler*> ready_actors; /**< Actors currently scheduled or running. */
        std::shared_mutex ready_actors_mutex; /**< Guards ready_actors. */
        std::atomic<bool> stop; /**< Set when shutdown() is requested. */
        std::mutex actor_locks[256]; /**< Lock striping for per-actor serialization. */

        /**
         * @brief Return the stripe mutex for an actor pointer.
         *
         * This implements lock striping: different actor pointers map to
         * different mutexes via a simple hash. The returned mutex must be
         * locked by callers that need to serialize operations for a single
         * actor (for example, to avoid races between submit() and complete()).
         *
         * @param actor Non-null pointer to the actor.
         * @return Reference to the mutex protecting that actor's stripe.
         * @throws std::invalid_argument if `actor` is nullptr.
         */
        std::mutex& get_actor_lock(IEventHandler* actor);

        /**
         * @brief Schedule `task` on the shared `Executor` and arrange for
         *        `complete(actor)` to be called when the task finishes.
         *
         * This forwards a callable to the backing `Executor`. Any exception
         * thrown by `Executor::execute` (for example, if the executor is
         * shut down or rejects the work) is propagated to the caller.
         *
         * @param actor Actor that owns the task (used for completion handling).
         * @param task Callable to run on a worker thread; captured/moved into
         *             the Executor.
         * @throws Whatever `Executor::execute` may throw (e.g. rejections).
         */
        void execute(std::shared_ptr<IEventHandler> actor, IO_Task task);

        /**
         * @brief Called after a scheduled task completes; schedules next task
         *        for the same actor if one exists, otherwise clears the actor
         *        from the ready list.
         *
         * @param actor Actor whose completed task triggered this call.
         * @throws std::bad_alloc if enqueuing or map operations allocate and fail.
         */
        void complete(std::shared_ptr<IEventHandler> actor);

        /**
         * @brief Return a reference to the pending task queue for `actor`.
         *
         * If no queue exists for the actor, one is created and a reference is
         * returned. The returned reference is valid until the actor is removed
         * via `remove_actor` or the map is otherwise modified in a way that
         * invalidates references.
         *
         * @param actor Non-null actor pointer whose queue is requested.
         * @return Reference to the actor's FIFO queue of pending tasks.
         * @throws std::invalid_argument if `actor` is nullptr.
         * @throws std::bad_alloc if the map needs to allocate a new queue.
         */
        std::queue<IO_Task>& pending_tasks_of(IEventHandler* actor);

    public:
        /**
         * @brief Construct an ActorThreadPool backed by `thread_num` workers.
         *
         * The constructor initializes the underlying `Executor` which may
         * create threads and allocate resources.
         *
         * @param thread_num Number of worker threads to create in the Executor.
         * @throws std::invalid_argument or other exceptions propagated from
         *         `Executor` construction (e.g., invalid thread count).
         * @throws std::bad_alloc on allocation failure.
         */
        ActorThreadPool(int thread_num);

        /**
         * @brief Destructor - initiates shutdown.
         *
         * The destructor calls `shutdown()` to stop accepting new tasks and
         * to begin draining the Executor.
         */
        ~ActorThreadPool();

        /**
         * @brief Submit a task for execution for a specific actor.
         *
         * If the actor does not currently have a running task, the submitted
         * task is scheduled immediately. If the actor is already running,
         * the task is appended to the actor's FIFO pending queue.
         *
         * @param actor Non-null shared pointer to the actor that owns the task.
         * @param task The callable to execute; it will be moved/copied into
         *             internal structures and executed on a worker thread.
         * @throws std::invalid_argument if `actor` is nullptr.
         * @throws std::bad_alloc if internal containers need to grow.
         * @throws Whatever `Executor::execute` may throw when scheduling work.
         */
        void submit(std::shared_ptr<IEventHandler> actor, IO_Task task);

        /**
         * @brief Stop accepting new work and shut down the backing Executor.
         *
         * After calling `shutdown()` the pool will not schedule new tasks on
         * the Executor; already submitted tasks remain queued and will run
         * or be drained according to the Executor's semantics.
         *
         * @throws Whatever `Executor::shutdown` propagates.
         */
        void shutdown();

        /**
         * @brief Remove all bookkeeping for `actor`.
         *
         * This erases the actor's pending queue (dropping any queued tasks)
         * and removes it from the ready list. Callers should ensure no tasks
         * are running for the actor or otherwise synchronize externally to
         * avoid races with running tasks.
         *
         * @param actor Non-null pointer to the actor to remove.
         * @throws std::invalid_argument if `actor` is nullptr.
         * @throws std::bad_alloc only if container operations require allocation.
         */
        void remove_actor(IEventHandler* actor);
    
        ActorThreadPool() = delete;
        ActorThreadPool(const ActorThreadPool&) = delete;
        ActorThreadPool& operator=(const ActorThreadPool&) = delete;
        ActorThreadPool(ActorThreadPool&&) = delete;
        ActorThreadPool& operator=(ActorThreadPool&&) = delete;

};

#endif