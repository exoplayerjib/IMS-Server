#include "connection_handler.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h> 
#include <cstring>
#include <errno.h>
#include <iostream>
#include <sys/epoll.h>
#include <stdexcept>
#include <string>
#include <algorithm>

ConnectionHandler::ConnectionHandler(int fd, Reactor* reactor) :
    fd(fd),
    current_write_offset(0),
    reactor(reactor),
    closed(false) {}

ConnectionHandler::~ConnectionHandler() {
    close(fd);
}

int ConnectionHandler::get_fd() {
    return fd;
}

bool ConnectionHandler::is_closed() const {
    return closed;
}

std::function<void()> ConnectionHandler::handle_read(){
    if (closed) {
        return nullptr;
    }
    while (true) {
        ssize_t bytes_read = recv(fd, current_read_bytestream.data(), BYTESTREAM_SIZE, 0);
        if (bytes_read > 0) {
            return [this, payload = std::vector<char>(current_read_bytestream.data(), current_read_bytestream.data() + bytes_read)]() mutable {
                std::cout << "Received message of size " << payload.size() << " bytes from fd " << fd << std::endl;
                return;
            };
        }
        else {
            handle_io_error(bytes_read);
            return nullptr;
        }
    }


}

void ConnectionHandler::handle_write() {
    if (closed) return;
    std::lock_guard<std::mutex> write_lock(write_queue_mutex);
    while (!write_queue.empty()) {
        const std::vector<char>& current_messageframe = write_queue.front();
        ssize_t bytes_written = send(fd, current_messageframe.data() + current_write_offset, current_messageframe.size() - current_write_offset, MSG_NOSIGNAL);
        if (bytes_written > 0) {
            current_write_offset += bytes_written;
            if (current_write_offset == current_messageframe.size()) {
                write_queue.pop();
                current_write_offset = 0;
            }
            else return;
        }
        else {
            handle_io_error(bytes_written);
            break;
        }
    }
    if (!closed && write_queue.empty()) {
        epoll_event event {};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = fd;
        reactor->update_ops(fd, event);
    }
}

void ConnectionHandler::handle_io_error(ssize_t bytes_read) {
    if (bytes_read == 0){ 
        #ifdef DEBUG
        std::cout << "received 0 bytes in recv - fd " << fd << " closed the connection" << std::endl;
        #endif
        closed = true;
    }
    else if (bytes_read == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK){
            return;
        }
        else {
            std::cerr << "Error in recv/send: errno: " << errno << std::endl;
            std::cerr << strerror(errno) << std::endl;
            closed = true;
        }
    }
}

void ConnectionHandler::send_message(const std::vector<char>& message) {
    if (closed) return;
    {
        std::lock_guard<std::mutex> write_lock(write_queue_mutex);
        write_queue.push(message);
    }
    epoll_event event {};
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
    event.data.fd = fd;
    reactor->update_ops(fd, event);
}
