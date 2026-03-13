#include <gtest/gtest.h>
#include "connection_handler.h"
#include "reactor.h"
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <memory>

// Fixture עבור בדיקות ה-Connection Handler
class ConnectionHandlerTest : public ::testing::Test {
protected:
    int sv[2]; // sv[0] = Mock Client, sv[1] = Server (ConnectionHandler)
    std::unique_ptr<Reactor> dummy_reactor;
    std::unique_ptr<ConnectionHandler> handler;

    void SetUp() override {
        // יצירת זוג שקעים מחוברים (ללא רשת אמיתית) שמוגדרים כ-Non-blocking
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);

        // יצירת Reactor דמה. אנחנו מנסים למצוא פורט פנוי (בסביבות 50,000)
        for (int port = 50000; port < 50100; ++port) {
            try {
                dummy_reactor = std::make_unique<Reactor>(1, port);
                break;
            } catch (...) {}
        }
        ASSERT_NE(dummy_reactor, nullptr) << "Failed to initialize dummy reactor on any port.";

        // אתחול ה-Handler עם צד השרת של ה-socketpair
        handler = std::make_unique<ConnectionHandler>(sv[1], dummy_reactor.get());
    }

    void TearDown() override {
        // סגירת צד הלקוח (צד השרת נסגר אוטומטית ב-destructor של ה-handler)
        if (sv[0] != -1) {
            close(sv[0]);
        }
    }

    // פונקציית עזר לשליחת מידע מדויק מה"לקוח" ל"שרת"
    void send_from_client(const char* data, size_t len) {
        ssize_t sent = send(sv[0], data, len, 0);
        ASSERT_EQ(sent, len);
    }
};

// ==========================================
// קבוצה 1: קריאת מידע תקינה (Reading)
// ==========================================

TEST_F(ConnectionHandlerTest, ReadsCompleteMessage) {
    // שלב 1: הלקוח שולח הודעה שלמה - גודל 5, ואז המילה HELLO
    uint32_t len = htonl(5); // המרה ל-Network Byte Order
    send_from_client((char*)&len, 4);
    send_from_client("HELLO", 5);

    // שלב 2: ה-Handler מנסה לקרוא
    auto task = handler->handle_read();
    
    // מצפים שהמשימה הוחזרה (הודעה הושלמה) והחיבור לא נסגר
    EXPECT_NE(task, nullptr);
    EXPECT_FALSE(handler->is_closed());
}

// ==========================================
// קבוצה 2: קיטוע מנות ברשת (Fragmentation)
// ==========================================

TEST_F(ConnectionHandlerTest, HandlesFragmentedMessage) {
    uint32_t len = htonl(6);
    char header[4];
    std::memcpy(header, &len, 4);

    // רשתות לא תמיד מעבירות הכל בבת אחת. נשלח רק חצי מה-Header:
    send_from_client(header, 2);
    EXPECT_EQ(handler->handle_read(), nullptr); // הוא אמור להבין שחסר מידע

    // נשלח את החצי השני של ה-Header
    send_from_client(header + 2, 2);
    EXPECT_EQ(handler->handle_read(), nullptr); // עכשיו הוא מצפה ל-Payload

    // נשלח רק אות אחת מה-Payload
    send_from_client("W", 1);
    EXPECT_EQ(handler->handle_read(), nullptr); // עדיין לא סיים

    // נשלח את שאר המילה
    send_from_client("ORLD!", 5);
    auto task = handler->handle_read();
    
    // רק עכשיו הוא אמור להחזיר לנו את הפונקציה לביצוע!
    EXPECT_NE(task, nullptr);
    EXPECT_FALSE(handler->is_closed());
}

// ==========================================
// קבוצה 3: אבטחה ושגיאות (Errors & Limits)
// ==========================================

TEST_F(ConnectionHandlerTest, ClosesOnOversizedPayload) {
    // ננסה להפיל את השרת עם הודעה בגודל 11 מגה-בייט (הגבלת המקסימום היא 10)
    uint32_t len = htonl(1024 * 1024 * 11);
    send_from_client((char*)&len, 4);

    auto task = handler->handle_read();
    
    // השרת אמור לזהות את החריגה ולסגור מיד את החיבור
    EXPECT_EQ(task, nullptr);
    EXPECT_TRUE(handler->is_closed());
}

TEST_F(ConnectionHandlerTest, HandlesClientDisconnect) {
    // הלקוח מתנתק בפתאומיות
    close(sv[0]);
    sv[0] = -1; // כדי לא לסגור שוב ב-TearDown

    auto task = handler->handle_read();
    
    // פונקציית recv תחזיר 0, ה-Handler אמור להבין שהחיבור נסגר
    EXPECT_EQ(task, nullptr);
    EXPECT_TRUE(handler->is_closed());
}

// ==========================================
// קבוצה 4: כתיבה ותור שליחה (Writing)
// ==========================================

TEST_F(ConnectionHandlerTest, WritesMessageToSocket) {
    // נייצר הודעה בצד השרת
    ByteStream msg;
    msg.expected_size = 9; // 4 ל-Header ועוד 5 ל-"WORLD"
    msg.buffer.resize(9);
    
    uint32_t len = htonl(5);
    std::memcpy(msg.buffer.data(), &len, 4);
    std::memcpy(msg.buffer.data() + 4, "WORLD", 5);

    // נכניס לתור הכתיבה (אמור לעדכן את ה-Epoll)
    handler->send_message(msg);
    
    // בגלל שזה non-blocking, ה-handle_write אמור לשפוך הכל מיד ל-Socket
    handler->handle_write();

    // נוודא שהלקוח אכן קיבל את המידע הנכון
    char recv_buf[10] = {0};
    ssize_t bytes_read = recv(sv[0], recv_buf, 9, 0); // קריאה בצד הלקוח
    ASSERT_EQ(bytes_read, 9);
    
    uint32_t recv_len;
    std::memcpy(&recv_len, recv_buf, 4);
    
    // מוודאים המרה תקינה בחזרה והגעת הטקסט
    EXPECT_EQ(ntohl(recv_len), 5);
    EXPECT_STREQ(recv_buf + 4, "WORLD");
}