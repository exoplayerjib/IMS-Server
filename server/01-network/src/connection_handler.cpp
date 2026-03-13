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

ConnectionHandler::ConnectionHandler(int fd, Reactor* reactor) :
    fd(fd),
    reactor(reactor),
    read_state(ReadState::READ_HEADER),
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
    if(read_state == ReadState::READ_HEADER)
        read_header();
    else if(read_state == ReadState::READ_PAYLOAD && current_read_bstream.total_bytes_processed < current_read_bstream.expected_size)
        read_payload();
    
    if (closed) return nullptr;

    if (read_state == ReadState::READ_PAYLOAD && current_read_bstream.total_bytes_processed == current_read_bstream.expected_size) {
        std::function<void()> continue_read = [this, payload_buffer = std::move(current_read_bstream.buffer)]() mutable 
        {
            // Process the payload here. For demonstration, we just print it.
            // implement this when the the TLS layer is implemented.
            // std::string payload_str(payload_buffer.begin() + MESSAGE_HEADER_SIZE, payload_buffer.end());
            std::cout << "Received payload with size " << payload_buffer.size() - MESSAGE_HEADER_SIZE << " bytes." << std::endl;
        };
        current_read_bstream = {};
        read_state = ReadState::READ_HEADER;
        return continue_read;
    }
    return nullptr;
}


void ConnectionHandler::read_header() {
    size_t bytes_left = MESSAGE_HEADER_SIZE - current_read_bstream.total_bytes_processed;
    char* cur_pos = current_read_bstream.buffer.data() + current_read_bstream.total_bytes_processed;
    ssize_t bytes_received = recv(fd, cur_pos, bytes_left, 0);
    if (bytes_received > 0){
        current_read_bstream.total_bytes_processed += bytes_received;
        if (current_read_bstream.total_bytes_processed == MESSAGE_HEADER_SIZE) {
            uint32_t network_payload_size;
            std::memcpy(&network_payload_size, current_read_bstream.buffer.data(), MESSAGE_HEADER_SIZE);
            uint32_t raw_payload_size = ntohl(network_payload_size);
            if (raw_payload_size > MAX_PAYLOAD_SIZE) {
                std::cerr << "Payload size " << raw_payload_size << " exceeds maximum allowed. Closing connection." << std::endl;
                closed = true;
                return;
            }
            current_read_bstream.expected_size = raw_payload_size + MESSAGE_HEADER_SIZE;
            current_read_bstream.buffer.resize(current_read_bstream.expected_size);
            read_state = ReadState::READ_PAYLOAD;
            read_payload();
        }
    }
    else {
        handle_io_error(bytes_received);
    }
}

void ConnectionHandler::read_payload(){
    size_t bytes_left = current_read_bstream.expected_size - current_read_bstream.total_bytes_processed;
    char* cur_pos = current_read_bstream.buffer.data() + current_read_bstream.total_bytes_processed;
    if (bytes_left == 0) return;
    ssize_t bytes_received = recv(fd, cur_pos, bytes_left, 0);
    if (bytes_received > 0){
        current_read_bstream.total_bytes_processed += bytes_received;
        if (current_read_bstream.total_bytes_processed == current_read_bstream.expected_size){
            return;
        }
    }
    else handle_io_error(bytes_received);
}

void ConnectionHandler::handle_write() {
    if (closed) return;
    std::lock_guard<std::mutex> write_lock(write_queue_mutex);
    while (!write_queue.empty()) {
        ByteStream& bstream = write_queue.front();
        if (bstream.total_bytes_processed < bstream.expected_size){
            size_t bytes_left = bstream.expected_size - bstream.total_bytes_processed;
            char* cur_pos = bstream.buffer.data() + bstream.total_bytes_processed;
            ssize_t bytes_sent = send(fd, cur_pos, bytes_left, MSG_NOSIGNAL);
            if (bytes_sent > 0){
                bstream.total_bytes_processed += bytes_sent;
                if (bstream.total_bytes_processed == bstream.expected_size) { 
                    write_queue.pop();
                }
                else{
                    return;
                }
            }
            else {
                handle_io_error(bytes_sent);
                break;
            }
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

void ConnectionHandler::send_message(const ByteStream& message) {
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


