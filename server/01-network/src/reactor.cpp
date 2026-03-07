#include "reactor.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "connection_handler.h"

Reactor::Reactor(int thread_num, int port) :  port(port) , thread_pool(thread_num) {
    if (thread_num <= 0) {
        std::cerr << "Thread number must be positive." << std::endl;
        exit(EXIT_FAILURE);
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "Port number must be between 1 and 65535." << std::endl;
        exit(EXIT_FAILURE);
    }
    server_fd = socket(AF_INET,SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd == -1){
        std::cerr << "Failed to create server socket." << std::endl;
        exit(EXIT_FAILURE);
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
        exit(EXIT_FAILURE);
    }

    #ifdef DEBUG
    std::cout << "Server socket bound to port: " << port << std::endl;
    #endif

    if(listen(server_fd, 128) == -1){
        std::cerr << "Failed to set the server socket to listen mode." << std::endl;
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    #ifdef DEBUG
    std::cout << "Server socket is now listening." << std::endl;
    #endif

    this->epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed." << std::endl;
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    #ifdef DEBUG
    std::cout << "Epoll instance created." << std::endl;
    #endif

    epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd,EPOLL_CTL_ADD,server_fd,&event) == -1){
        std::cerr << "Failed to add server socket to epoll in EPOLLIN mode." << std::endl;
        close(server_fd);
        close(epoll_fd);
        exit(EXIT_FAILURE);
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
}

void Reactor::register_connection(std::shared_ptr<IEventHandler> handler){ 
    int fd = handler->get_fd();
    epoll_event event;
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
        while (true) {
            epoll_event events[64];
            int num_events = epoll_wait(epoll_fd, events, 64, -1);
            exec_reactor_tasks();
            if (num_events == -1) {
                std::cerr << "epoll_wait failed." << std::endl;
                continue;
            }
            for (int i = 0; i < num_events; i++){
                int fd = events[i].data.fd;
                if (fd == server_fd) {
                    struct sockaddr_in client_addr = {0}; 
                    socklen_t c_len = sizeof(client_addr); 
                    int client_con_fd = accept4(server_fd, (sockaddr*)&client_addr, &c_len, SOCK_NONBLOCK);
                    if (client_con_fd == -1){
                        std::cerr << "Failed to create connection fd to client." << std::endl;
                        continue;
                    }
                    ConnectionHandler* handler = new ConnectionHandler(client_con_fd);
                    std::shared_ptr<IEventHandler> handler_ptr(handler, [this,handler] 
                        {
                        this->thread_pool.remove_actor(handler);
                        delete handler;
                        });
                    register_connection(handler_ptr);
                }
                else {
                    if (handlers.find(fd) != handlers.end()) {
                        if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                           #ifdef DEBUG
                           std::cout << "Client disconnected or error on fd: " << fd << std::endl;
                           #endif
                           epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                           handlers.erase(fd);
                           continue;
                       }

                       if (events[i].events & EPOLLIN) {
                           handlers[fd]->handle_read();
                       }
                       if (events[i].events & EPOLLOUT) {
                           handlers[fd]->handle_write();
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
    }
}
