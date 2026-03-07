#ifndef REACTOR_H
#define REACTOR_H
#include <string>
#include <vector>
#include <thread>
#include <unordered_map>
#include "eventhandler.h"
#include "actor_thread_pool.h"
#include <sys/epoll.h>
#include <functional>
#include <mutex>
#include <memory>
#include <atomic>

typedef std::function<void()> UpdateTask; // update tasks.

class Reactor {
    private:
        int port;
        int epoll_fd;
        int server_fd;
        int wakeup_fd;
        std::atomic<bool> running {true};
        ActorThreadPool thread_pool;
        std::unordered_map<int, std::shared_ptr<IEventHandler>> handlers;
        std::thread::id main_thread;
        std::vector<UpdateTask> update_queue;
        std::mutex update_queue_mutex;

        /*
        * Execute all tasks in the update queue.
        * This is called in the main event loop to ensure that updates to the epoll instance
        * are performed in a thread-safe manner.
        */
        void exec_reactor_tasks();
        /*
        * Register a new connection to the epoll with EPOLLIN.
        * @param handler The event handler for the connection.
        */
        void register_connection(std::shared_ptr<IEventHandler> handler);


    public:
        /*
        * Constructor for the Reactor class.
        * @param thread_num The number of threads to use for handling events.
        * @param port The port on which the server will listen.
        */
        Reactor(int thread_num, int port);
        ~Reactor();
        /*
        * Start the reactor event loop.
        */
        void start();

        /**
         * Update the operations for a file descriptor in the epoll instance.
         * @param fd The file descriptor to update.
         * @param event The epoll event to update with.
         */
        void update_ops(int fd, epoll_event& event);

        /**
         * Wake up the reactor's event loop, causing it to re-check for events.
         * This is used to ensure that updates to the epoll instance are processed promptly.
         */
        void wakeup();


        /*
        * Shutdown the reactor and clean up resources.
         * This will stop the event loop and close all file descriptors.
         * After calling this method, the reactor should not be used again.
         * @throws std::runtime_error if shutdown fails.   
        */
        void shutdown();

        Reactor(const Reactor&) = delete;
        Reactor& operator=(const Reactor&) = delete;
        Reactor(Reactor&&) = delete;
        Reactor& operator=(Reactor&&) = delete;
        
};

#endif