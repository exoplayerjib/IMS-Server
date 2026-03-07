#include "actor_thread_pool.h"
#include <iostream>

ActorThreadPool::ActorThreadPool(int thread_num) : threadpool(thread_num), stop(false) {}

ActorThreadPool::~ActorThreadPool() {
    this->shutdown();
}

std::mutex& ActorThreadPool::get_actor_lock(IEventHandler* actor) {
    if (actor == nullptr) {
        throw std::invalid_argument("Actor pointer cannot be null.");
    }
    int hashval = std::hash<IEventHandler*>{}(actor);
    return actor_locks[hashval%256];
}

void ActorThreadPool::shutdown() {
    stop = true;
    threadpool.shutdown();
}

void ActorThreadPool::remove_actor(IEventHandler* actor) {
    if (actor == nullptr) {
        throw std::invalid_argument("Actor pointer cannot be null.");
    }
    std::mutex& actor_mutex = get_actor_lock(actor);
    std::unique_lock<std::mutex> actor_lock(actor_mutex);
    {
        std::unique_lock<std::shared_mutex> maplock(actor_tasks_mutex);
        actor_tasks.erase(actor);
    }
    {
        std::unique_lock<std::shared_mutex> queuelock(ready_actors_mutex);
        ready_actors.erase(std::remove(ready_actors.begin(), ready_actors.end(), actor), ready_actors.end());
    }
}

void ActorThreadPool::submit(std::shared_ptr<IEventHandler> actor, IO_Task task){
    if (actor == nullptr) {
        throw std::invalid_argument("Actor pointer cannot be null.");
    }
    if (stop) {
        throw std::runtime_error("Cannot submit task to ActorThreadPool after shutdown.");
    }
    std::mutex& actor_mutex =  get_actor_lock(actor.get());
    std::unique_lock<std::mutex> actor_lock(actor_mutex);
    std::vector<IEventHandler*>::iterator it;
    bool is_ready = false;
    {
        std::shared_lock<std::shared_mutex> find_lock(ready_actors_mutex);
        is_ready = (std::find(ready_actors.begin(), ready_actors.end(),actor.get()) != ready_actors.end());
    }

    if (is_ready) {
        std::queue<IO_Task>& qu = pending_tasks_of(actor.get());
        std::unique_lock<std::shared_mutex> write_lock(actor_tasks_mutex);
        qu.emplace(task);
    }
    else {
        {
            std::unique_lock<std::shared_mutex> update_ra(ready_actors_mutex);
            ready_actors.push_back(actor.get());
        }
        try {
            execute(actor, task);
        }
        catch (const std::exception& e) {
            std::cerr << "Exception during task submission: " << e.what() << std::endl;
            // Roll back ready state on failure to submit
            std::unique_lock<std::shared_mutex> rollback_ra(ready_actors_mutex);
            ready_actors.erase(std::remove(ready_actors.begin(), ready_actors.end(), actor.get()), ready_actors.end());
            throw; // rethrow after cleanup
        }
    }
}

std::queue<IO_Task>& ActorThreadPool::pending_tasks_of(IEventHandler* actor){
    std::shared_lock<std::shared_mutex> read_lock(actor_tasks_mutex);
    if( auto it = actor_tasks.find(actor); it != actor_tasks.end()){
        return it->second;
    }
    else{
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(actor_tasks_mutex);
        return actor_tasks[actor];
    }
}

void ActorThreadPool::execute(std::shared_ptr<IEventHandler> actor, IO_Task task) {
    threadpool.execute([this, actor, task](){
        try {
            task();
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in task: " << e.what() << std::endl;
        }
        catch (...) {
            std::cerr << "Unknown exception in task." << std::endl;
        }
        complete(actor);
    });
}

void ActorThreadPool::complete(std::shared_ptr<IEventHandler> actor){
    if (actor == nullptr) {
        throw std::invalid_argument("Actor pointer cannot be null.");
    }
    std::mutex& actor_mutex = get_actor_lock(actor.get());
    std::unique_lock<std::mutex> actor_lock(actor_mutex);
    {
        std::shared_lock<std::shared_mutex> check_lock(actor_tasks_mutex);
        if (actor_tasks.find(actor.get()) == actor_tasks.end()) {
            return; // האקטור נמחק, פשוט יוצאים
        }
    }    
    std::queue<IO_Task>& pend = pending_tasks_of(actor.get());
    if (pend.empty()){
        std::unique_lock<std::shared_mutex> write_lock(ready_actors_mutex);
        ready_actors.erase(std::remove(ready_actors.begin(),ready_actors.end(),actor.get()), ready_actors.end());
    }
    else{
        IO_Task next_task = std::move(pend.front());
        pend.pop();
        execute(actor, next_task);
    }
}