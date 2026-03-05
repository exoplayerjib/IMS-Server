#ifndef EVENTHANDLER_H
#define EVENTHANDLER_H

class IEventHandler {
    public:
        virtual void handle_read() = 0;
        virtual void handle_write() = 0;
        virtual int get_fd() = 0;
};

#endif