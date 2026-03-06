#include "executor.h"
#include <iostream>

Executor::Executor(int thread_num) {
    if (thread_num <= 0) {
        throw std::invalid_argument("Thread number must be positive.");
    }
    for (int i=0; i < thread_num; i++) {
        threads.emplace_back(
            [this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        condition.wait(lock, [this] {return stop || !this->tasks.empty();});
                        if(stop && tasks.empty()) return;
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    try{
                    task();
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Exception in task: " << e.what() << std::endl;
                    }
                    catch (...) {
                        std::cerr << "Unknown exception in task." << std::endl;
                    }
                }
            }
        
        );
    }
}

Executor::~Executor() {
    shutdown();
}

void Executor::execute(std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("Task cannot be empty.");
    }
    std::unique_lock<std::mutex> lock(queue_mutex);
    if (stop) {
        throw std::runtime_error("Cannot submit task to stopped Executor.");
    }
    tasks.emplace(task);
    condition.notify_one();
}

void Executor::shutdown(){
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread &t : threads){
        if(t.joinable()){
            t.join();
        }
    }
}

