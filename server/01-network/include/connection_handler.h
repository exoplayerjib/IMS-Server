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

/// Maximum size of the read buffer for incoming data.
#define BYTESTREAM_SIZE 4096

/**
 * @brief Non-blocking event handler for a single accepted TCP connection.
 *
 * @c ConnectionHandler implements the @c IEventHandler interface and is driven
 * by the @c Reactor event loop via epoll.
 *
 * On a read event, it reads available data from the socket into a fixed-size buffer
 * and returns a continuation task containing the data for processing on a worker thread.
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

        int fd;                              ///< File descriptor for the accepted client socket.
        std::array<char, BYTESTREAM_SIZE> current_read_bytestream;     ///< Buffer for assembling incoming message frames.
        std::queue<std::vector<char>> write_queue;   ///< Queue of complete message frames awaiting transmission to the client.
        size_t current_write_offset;            ///< Tracks progress of the current message being sent from the front of @c write_queue.
        std::mutex write_queue_mutex;        ///< Guards @c write_queue for concurrent producers.
        Reactor* reactor;                    ///< Non-owning pointer to the parent reactor used for epoll updates.
        bool closed;                         ///< Set to @c true when the peer closes the connection or an unrecoverable error occurs.

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
         * @brief Reads data from the socket on an @c EPOLLIN event.
         *
         * Reads up to @c BYTESTREAM_SIZE bytes from the file descriptor.
         * The read data is moved into a returned continuation that the @c Reactor 
         * can dispatch to a worker thread.
         *
         * @return A callable containing the received data; @c nullptr on error
         *         or if the connection is closed.
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
        void send_message(const std::vector<char>& message);
};

#endif // CONNECTION_HANDLER_H