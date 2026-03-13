#ifndef CONNECTION_HANDLER_H
#define CONNECTION_HANDLER_H
#include "eventhandler.h"
#include <vector>
#include <array>
#include <functional>
#include "reactor.h"
#include <queue>
#include <mutex>
#include <cstdint>

/// Size in bytes of the fixed-length message header that precedes every payload.
#define MESSAGE_HEADER_SIZE 4
#define MAX_PAYLOAD_SIZE (1024 * 1024 * 10) // 10 MiB maximum payload size to prevent abuse

/**
 * @brief Holds the state of an in-progress read or write operation on a socket.
 *
 * Each message on the wire is framed as a 4-byte big-endian length prefix
 * followed by the payload bytes. A @c ByteStream tracks how many bytes of that
 * combined frame have been transferred so far, allowing partial reads/writes to
 * be resumed across multiple epoll wakeups.
 *
 * @note On construction the internal buffer is pre-allocated to
 *       @c MESSAGE_HEADER_SIZE bytes so the first @c recv call goes directly
 *       into a ready buffer.
 */
struct ByteStream {
    std::vector<char> buffer;          ///< Raw byte buffer holding header + payload data.
    size_t total_bytes_processed;      ///< Number of bytes transferred so far.
    uint32_t expected_size;            ///< Total frame size (header + payload) in bytes, populated once the header is fully received.
    ByteStream() : buffer(MESSAGE_HEADER_SIZE), total_bytes_processed(0), expected_size(0) {}
};

/**
 * @brief Non-blocking event handler for a single accepted TCP connection.
 *
 * @c ConnectionHandler implements the @c IEventHandler interface and is driven
 * by the @c Reactor event loop via epoll. It uses a two-phase length-prefixed
 * framing protocol:
 *
 * 1. **Header phase** – reads exactly @c MESSAGE_HEADER_SIZE (4) bytes that
 *    encode the payload length as a network-byte-order @c uint32_t.
 * 2. **Payload phase** – reads exactly that many bytes of payload.
 *
 * Once a complete message is assembled, @c handle_read() returns a
 * @c std::function<void()> continuation that the @c Reactor dispatches to a
 * worker thread for processing, keeping I/O multiplexing off the hot path.
 *
 * Outbound messages are enqueued via the write queue and drained lazily by
 * @c handle_write() when epoll signals @c EPOLLOUT. After the queue is
 * empty the handler downgrades the epoll interest back to @c EPOLLIN only.
 *
 * The class is non-copyable and non-movable; ownership is managed through
 * @c std::shared_ptr inside the @c Reactor.
 */
class ConnectionHandler : public IEventHandler {
    private:
        /**
         * @brief Tracks whether the handler is currently reading the header or
         *        the payload portion of a message frame.
         */
        enum class ReadState { READ_HEADER, READ_PAYLOAD };

        int fd;                              ///< File descriptor for the accepted client socket.
        ByteStream current_read_bstream;     ///< Accumulates bytes for the message currently being received.
        std::queue<ByteStream> write_queue;  ///< Ordered queue of outbound frames pending transmission.
        std::mutex write_queue_mutex;        ///< Guards @c write_queue for concurrent producers.
        Reactor* reactor;                    ///< Non-owning pointer to the parent reactor used for epoll updates.
        ReadState read_state;                ///< Current framing phase for inbound data.
        bool closed;                         ///< Set to @c true when the peer closes the connection or an unrecoverable error occurs.

        /**
         * @brief Attempts to read the 4-byte length header from the socket.
         *
         * If the full header is received the expected frame size is calculated,
         * the buffer is resized to fit header + payload, and the state machine
         * advances to @c READ_PAYLOAD, immediately attempting a payload read.
         */
        void read_header();

        /**
         * @brief Attempts to read remaining payload bytes from the socket.
         *
         * Called after the header has been fully received. Returns when the
         * payload is complete or when @c recv would block (@c EAGAIN /
         * @c EWOULDBLOCK).
         */
        void read_payload();

        /**
         * @brief Centralised error handler for @c recv / @c send return values.
         *
         * - @c 0: peer performed an orderly shutdown; sets @c closed.
         * - @c -1 with @c EAGAIN / @c EWOULDBLOCK: transient, returns immediately.
         * - @c -1 with any other errno: logs the error and sets @c closed.
         *
         * @param bytes_read Return value from @c recv or @c send.
         */
        void handle_io_error(ssize_t bytes_read);

    public:
        /**
         * @brief Constructs a handler for an already-accepted client socket.
         *
         * @param fd      Connected socket file descriptor (must be non-blocking).
         * @param reactor Pointer to the owning @c Reactor; must outlive this object.
         */
        ConnectionHandler(int fd, Reactor* reactor);

        /**
         * @brief Closes the underlying socket file descriptor.
         */
        ~ConnectionHandler();

        ConnectionHandler(const ConnectionHandler&) = delete;
        ConnectionHandler& operator=(const ConnectionHandler&) = delete;
        ConnectionHandler(ConnectionHandler&&) = delete;
        ConnectionHandler& operator=(ConnectionHandler&&) = delete;

        /**
         * @brief Drives the inbound framing state machine on an @c EPOLLIN event.
         *
         * Reads as many bytes as are available without blocking, advancing
         * through the header and payload phases. When a complete message frame
         * has been assembled the buffer is moved into a returned continuation
         * that the @c Reactor can dispatch to a worker thread.
         *
         * @return A callable containing the fully-received payload if a complete
         *         message was assembled this call; @c nullptr otherwise (partial
         *         read, connection closed, or error).
         */
        std::function<void()> handle_read() override;

        /**
         * @brief Drains the outbound write queue on an @c EPOLLOUT event.
         *
         * Sends buffered frames in order, resuming partial sends on the next
         * wakeup. Once the queue is empty the epoll interest mask is downgraded
         * to @c EPOLLIN | @c EPOLLRDHUP to avoid spurious wakeups.
         */
        void handle_write() override;

        /**
         * @brief Returns the client socket file descriptor.
         * @return The file descriptor passed to the constructor.
         */
        int get_fd() override;

        /**
         * @brief Indicates whether the connection has been closed.
         * @return @c true if the peer disconnected or an unrecoverable I/O error
         *         occurred; @c false otherwise.
         */
        bool is_closed() const override;
        
        /// @brief Enqueues a message for sending to the client. and updates epoll interest to include @c EPOLLOUT.
        /// @param message The complete message frame (including header) to send.
        void send_message(const ByteStream& message);
};

#endif // CONNECTION_HANDLER_H