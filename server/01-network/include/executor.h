#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <thread>
#include <vector>
#include <queue>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <atomic>

class Executor {
    private:
        std::vector<std::thread> threads;
        std::queue<std::function<void()>> tasks;
        std::mutex queue_mutex;
        std::condition_variable condition;
        std::atomic<bool> stop = false;

    public:
        Executor(int thread_num);
        ~Executor();

        void execute(std::function<void()> task);
        void shutdown();
        Executor() = delete;
        Executor(const Executor&) = delete;
        Executor& operator=(const Executor&) = delete;
        Executor(Executor&&) = delete;
        Executor& operator=(Executor&&) = delete;
};


#endif