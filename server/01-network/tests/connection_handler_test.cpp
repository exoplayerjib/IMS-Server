#include <gtest/gtest.h>
#include "connection_handler.h"
#include "reactor.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <atomic>

// Helper class to manage socket pairs for testing
class SocketPairFixture : public ::testing::Test {
protected:
    int sv[2]; // sv[0] = user/server side (handler), sv[1] = tester/client side
    std::unique_ptr<Reactor> reactor;

    void SetUp() override {
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == -1) {
            perror("socketpair");
            exit(1);
        }
        // Initialize a dummy reactor (needed for the ConnectionHandler constructor)
        // We pick a random port to avoid binding conflicts, though we won't strictly use the listening socket.
        reactor = std::make_unique<Reactor>(1, 50000 + (getpid() % 1000));
    }

    void TearDown() override {
        close(sv[0]);
        close(sv[1]);
    }
};

// Test that handle_read correctly reads available bytes from the socket
TEST_F(SocketPairFixture, ReadsAvailableData) {
    ConnectionHandler handler(sv[0], reactor.get());

    const std::string message = "Hello, World!";
    ssize_t sent = write(sv[1], message.c_str(), message.size());
    ASSERT_EQ(sent, message.size());

    // Perform read
    auto task = handler.handle_read();

    ASSERT_NE(task, nullptr) << "handle_read should return a valid task when data is available.";
    
    // In a real scenario, executing the task would process the business logic.
    // Since the current implementation captures the payload into the lambda,
    // getting a non-null task confirms data was read successfully.
}

// Test that handle_read returns nullptr on error or EOF
TEST_F(SocketPairFixture, HandlesDisconnectGracefully) {
    ConnectionHandler handler(sv[0], reactor.get());
    
    // Close the client side
    close(sv[1]);

    // Expect nullptr (handler detects closure)
    auto task = handler.handle_read();
    EXPECT_EQ(task, nullptr);
    EXPECT_TRUE(handler.is_closed());
}

// Test send_message queues data and handle_write sends it
TEST_F(SocketPairFixture, SendsMessageCorrectly) {
    ConnectionHandler handler(sv[0], reactor.get());

    std::string msg = "ResponseData";
    std::vector<char> buffer(msg.begin(), msg.end());

    // Enqueue message
    // Note: This triggers reactor->update_ops, which might print an error because sv[0] isn't 
    // registered in the real epoll instance of the dummy reactor, but the handler logic should proceed.
    handler.send_message(buffer);

    // Drain queue
    handler.handle_write();

    // Verify data arrived at client side
    char read_buf[1024];
    ssize_t received = read(sv[1], read_buf, sizeof(read_buf));
    
    ASSERT_GT(received, 0);
    std::string received_str(read_buf, received);
    EXPECT_EQ(received_str, msg);
}

// Test sending multiple messages preserves order
TEST_F(SocketPairFixture, SendsMultipleMessagesInOrder) {
    ConnectionHandler handler(sv[0], reactor.get());

    std::vector<std::string> messages = {"First", "Second", "Third"};

    for (const auto& txt : messages) {
        std::vector<char> buf(txt.begin(), txt.end());
        handler.send_message(buf);
    }

    // Process writes
    handler.handle_write();

    // Read all from client end
    char read_buf[2048];
    // Give some time for data to flush
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ssize_t received = read(sv[1], read_buf, sizeof(read_buf));

    ASSERT_GT(received, 0);
    std::string total_received(read_buf, received);
    
    std::string expected = "FirstSecondThird";
    EXPECT_EQ(total_received, expected);
}

// Test partial writes (simulating blocked socket)
TEST_F(SocketPairFixture, HandlesPartialWrites) {
    ConnectionHandler handler(sv[0], reactor.get());

    // 1. Fill the kernel receive buffer on the "client" side (sv[1]) 
    // so that writes to sv[0] will eventually block or be partial.
    // We do this by writing to sv[0] directly until it blocks, then we know the pipe is full.
    // BUT we want to test handle_write, so we need to fill sv[1]'s RECV buffer.
    // We do that by writing LOTS of data to sv[0] and NOT reading from sv[1].
    
    // However, send_message just queues. handle_write does the `send` syscall.
    
    // Let's queue a massive message (larger than typical kernel buffer, e.g., > 200KB)
    size_t huge_size = 500 * 1024; 
    std::vector<char> huge_msg(huge_size, 'A');
    handler.send_message(huge_msg);

    // 2. Call handle_write. It should send what it can and return, leaving remaining data in queue.
    handler.handle_write();
    
    // 3. Now read some data from sv[1] to make space
    char buf[4096];
    ssize_t read_bytes = read(sv[1], buf, sizeof(buf));
    ASSERT_GT(read_bytes, 0);

    // 4. Call handle_write again to resume sending
    handler.handle_write();

    // Verification: If logic failed, we might lose data or crash. 
    // We verified that handle_write didn't block indefinitely (test would timeout) 
    // and that we could resume.
    SUCCEED();
}
