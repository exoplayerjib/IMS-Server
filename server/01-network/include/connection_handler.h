#ifndef CONNECTION_HANDLER_H
#define CONNECTION_HANDLER_H
#include "eventhandler.h"

class ConnectionHandler : public IEventHandler {
    private:
        int fd;

    public:
        ConnectionHandler(int fd) : fd(fd) {}
        ~ConnectionHandler(); // make sure to delete the fd
        ConnectionHandler(const ConnectionHandler&) = delete;
        ConnectionHandler& operator=(const ConnectionHandler&) = delete;
        ConnectionHandler(ConnectionHandler&&) = delete;
        ConnectionHandler& operator=(ConnectionHandler&&) = delete;

        void handle_read() override;
        void handle_write() override;
        int get_fd() override { return fd; }
};

#endif // CONNECTION_HANDLER_H