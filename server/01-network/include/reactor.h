#ifndef REACTOR_H
#define REACTOR_H
#include <string>
#include <vector>
#include <thread>
#include <unordered_map>
#include "eventhandler.h"
#include <sys/epoll.h>
#include <functional>
#include <mutex>


typedef std::function<void()> UpdateTask; // update tasks.

class Reactor {
    private:
        int thread_num;
        int port;
        int epoll_fd;
        int server_fd;
        std::unordered_map<int, IEventHandler*> handlers;
        std::thread::id main_thread;
        std::vector<UpdateTask> update_queue;
        std::mutex update_queue_mutex;

        /*
        * Execute all tasks in the update queue.
        * This is called in the main event loop to ensure that updates to the epoll instance
        * are performed in a thread-safe manner.
        */
        void exec_update_queue();
        /*
        * Register a new connection to the epoll with EPOLLIN.
        * @param handler The event handler for the connection.
        */
        void register_connection(IEventHandler* handler);


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

};

#endif