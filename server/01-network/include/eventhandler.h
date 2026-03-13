#ifndef EVENTHANDLER_H
#define EVENTHANDLER_H
#include <functional>

class IEventHandler {
    public:
        virtual std::function<void()> handle_read() = 0;
        virtual void handle_write() = 0;
        virtual int get_fd() = 0;
        virtual ~IEventHandler() = default;
        virtual bool is_closed() const = 0;
};

#endif