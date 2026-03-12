#ifndef CONNECTION_HANDLER_H
#define CONNECTION_HANDLER_H
#include "eventhandler.h"
#include <vector>
#include <array>
#include <functional>
#include "reactor.h"
#include <queue>
#include <shared_mutex>
#include <cstdint>

#define MESSAGE_HEADER_SIZE 4

struct ByteStream {
    std::vector<char> buffer;
    size_t total_bytes_processed; 
    uint32_t expected_size;
    ByteStream() : buffer(MESSAGE_HEADER_SIZE), total_bytes_processed(0), expected_size(0) {}
};

class ConnectionHandler : public IEventHandler {
    private:
    /* packets consist of a 4-byte header that state the payload size, followed by the payload itself. 
    */
        enum class ReadState { READ_HEADER, READ_PAYLOAD };
        int fd; // File descriptor for the connection socket
        ReadState read_state; // Current state of reading (header or payload)
        ByteStream current_read_bstream;
        std::queue<ByteStream> write_queue;
        std::shared_mutex write_queue_mutex;
        Reactor* reactor;
        bool closed;

        void read_header();
        void read_payload();
        void handle_io_error(ssize_t bytes_read);

    public:
        ConnectionHandler(int fd, Reactor* reactor);
        ~ConnectionHandler(); 

        ConnectionHandler(const ConnectionHandler&) = delete;
        ConnectionHandler& operator=(const ConnectionHandler&) = delete;
        ConnectionHandler(ConnectionHandler&&) = delete;
        ConnectionHandler& operator=(ConnectionHandler&&) = delete;

        std::function<void()> handle_read() override;
        void handle_write() override;
        int get_fd() override;
        bool is_closed() const override;
};

#endif // CONNECTION_HANDLER_H