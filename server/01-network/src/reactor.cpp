#include "reactor.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "connection_handler.h"
#include <sys/eventfd.h>
#include <stdexcept>

Reactor::Reactor(int thread_num, int port) :  port(port) , thread_pool(thread_num) {
    if (thread_num <= 0) {
        std::cerr << "Thread number must be positive." << std::endl;
        throw std::runtime_error("Invalid thread number.");
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "Port number must be between 1 and 65535." << std::endl;
        throw std::runtime_error("Invalid port number.");
    }
    server_fd = socket(AF_INET,SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd == -1){
        std::cerr << "Failed to create server socket." << std::endl;
        throw std::runtime_error("Failed to create server socket.");
    }
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    #ifdef DEBUG
    std::cout << "Server socket created on port: " << port << std::endl;
    #endif
    
    if (bind(server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) == -1){
        std::cerr << "Server socket could not bind to address." << std::endl;
        close(server_fd);
        throw std::runtime_error("Failed to bind server socket.");
    }

    #ifdef DEBUG
    std::cout << "Server socket bound to port: " << port << std::endl;
    #endif

    if(listen(server_fd, 128) == -1){
        std::cerr << "Failed to set the server socket to listen mode." << std::endl;
        close(server_fd);
        throw std::runtime_error("Failed to set server socket to listen mode.");
    }

    #ifdef DEBUG
    std::cout << "Server socket is now listening." << std::endl;
    #endif

    this->epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed." << std::endl;
        close(server_fd);
        throw std::runtime_error("Failed to create epoll instance.");
    }
    
    #ifdef DEBUG
    std::cout << "Epoll instance created." << std::endl;
    #endif

    wakeup_fd = eventfd(0, EFD_NONBLOCK);
    if (wakeup_fd == -1) {
        std::cerr << "Failed to create wakeup eventfd." << std::endl;
        close(server_fd);
        close(epoll_fd);
        throw std::runtime_error("Failed to create wakeup eventfd.");
    }

    epoll_event event {};
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD ,server_fd, &event) == -1){
        std::cerr << "Failed to add server socket to epoll in EPOLLIN mode." << std::endl;
        close(server_fd);
        close(epoll_fd);
        throw std::runtime_error("Failed to add server socket to epoll.");
    }

    epoll_event wakeup_event {};
    wakeup_event.events = EPOLLIN;
    wakeup_event.data.fd = wakeup_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wakeup_fd, &wakeup_event) == -1) {
        std::cerr << "Failed to add wakeup eventfd to epoll." << std::endl;
        close(server_fd);
        close(epoll_fd);
        close(wakeup_fd);
        throw std::runtime_error("Failed to add wakeup eventfd to epoll.");
    }

    #ifdef DEBUG
    std::cout << "Server socket added to epoll instance." << std::endl;
    #endif

    #ifdef DEBUG
    std::cout << "Reactor initialized with thread pool of size: " << thread_num << std::endl;
    #endif
}

Reactor::~Reactor() {
    close(server_fd);
    close(epoll_fd);
    close(wakeup_fd);
}

void Reactor::shutdown() {
    if (!running) {
        return; // Already shut down
    }
    running = false;
    wakeup(); // Wake up the event loop to exit
}

void Reactor::wakeup() {
    uint64_t one = 1;
    ssize_t result = write(wakeup_fd, &one, sizeof(one));
    if (result == -1) {
        std::cerr << "Failed to write to wakeup eventfd." << std::endl;
    }
}

void Reactor::register_connection(std::shared_ptr<IEventHandler> handler){ 
    int fd = handler->get_fd();
    epoll_event event {};
    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
        std::cerr << "Failed to add socket to epoll." << std::endl;
        return;
    }
    handlers[fd] = handler;
    #ifdef DEBUG
    std::cout << "Handler registered for fd: " << fd << std::endl;
    #endif
}

void Reactor::start() {
    this->main_thread = std::this_thread::get_id();
    try{
        while (running) {
            epoll_event events[64];
            int num_events = epoll_wait(epoll_fd, events, 64, -1);
            exec_reactor_tasks();
            if (num_events == -1) {
                std::cerr << "epoll_wait failed." << std::endl;
                continue;
            }
            for (int i = 0; i < num_events; i++){
                int fd = events[i].data.fd;
                if (fd == wakeup_fd) {
                    uint64_t val;
                    read(wakeup_fd, &val, sizeof(val)); 
                    continue;
                }
                if (fd == server_fd) {
                    struct sockaddr_in client_addr = {0}; 
                    socklen_t c_len = sizeof(client_addr); 
                    int client_con_fd = accept4(server_fd, (sockaddr*)&client_addr, &c_len, SOCK_NONBLOCK);
                    if (client_con_fd == -1){
                        std::cerr << "Failed to create connection fd to client." << std::endl;
                        continue;
                    }
                    ConnectionHandler* handler = new ConnectionHandler(client_con_fd, this);
                    std::shared_ptr<IEventHandler> handler_ptr(handler, [this,handler] 
                        {
                        this->thread_pool.remove_actor(handler);
                        delete handler;
                        });
                    register_connection(handler_ptr);
                }
                else {
                    auto it = handlers.find(fd);
                    if (it != handlers.end()) {
                        if (events[i].events & EPOLLIN) {
                            IO_Task read_task = it->second->handle_read();
                            if (read_task) {
                                thread_pool.submit(it->second, read_task);
                            }
                        }
                        if (it->second->is_closed() || events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                           #ifdef DEBUG
                           std::cout << "Client disconnected or error on fd: " << fd << std::endl;
                           #endif
                           epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                           handlers.erase(fd);
                           continue;
                        }  
                        if (events[i].events & EPOLLOUT) {
                           it->second->handle_write();
                        }
                        if (it->second->is_closed()) {
                            #ifdef DEBUG
                            std::cout << "Client disconnected after write on fd: " << fd << std::endl;
                            #endif
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                            handlers.erase(fd);
                        }
                   }
                }   
            }
        }
    }
    catch(const std::exception& e){
        std::cerr << "Exception in Reactor::start: " << e.what() << std::endl;
    }
}

void Reactor::exec_reactor_tasks(){
    std::vector<UpdateTask> current_tasks; 
    {
        std::lock_guard<std::mutex> lock(update_queue_mutex);
        current_tasks = std::move(update_queue);
        update_queue.clear();
    }
    for (const UpdateTask& task : current_tasks) {
        try{
            task();
        }
        catch (std::exception& e){
            std::cerr << "Exception in executing update task: " << e.what() << std::endl;
            continue;
        }
    }
}

void Reactor::update_ops(int fd, epoll_event& event){
    if (std::this_thread::get_id() == main_thread) {
        if(handlers.find(fd) == handlers.end()){
            std::cerr << "Attempting to update ops for fd that is not registered: " << fd << std::endl;
            return;
        }
        if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1){
            std::cerr << "Failed to update ops for fd: " << fd << std::endl;
        }
    }
    else{
        { 
            std::lock_guard<std::mutex> lock(update_queue_mutex);
            update_queue.push_back([this ,fd, event]() mutable 
            {
                if(handlers.find(fd) == handlers.end()){
                    std::cerr << "Attempting to update ops for fd that is not registered: " << fd << std::endl;
                    return;
                }
                if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1){
                    std::cerr << "Failed to update ops for fd: " << fd << std::endl;
                }
            });
        }
        wakeup();
    }
}

void Reactor::close_connection(int fd) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    handlers.erase(fd);
}
