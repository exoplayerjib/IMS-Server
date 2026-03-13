#include <gtest/gtest.h>
#include "reactor.h"
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>

// משתנה גלובלי שמבטיח שכל טסט יקבל פורט ייחודי. 
// אם נריץ כמה טסטים על אותו פורט מהר מדי, מערכת ההפעלה עלולה לחסום אותנו (TIME_WAIT).
static std::atomic<int> next_test_port{45000};

class ReactorTest : public ::testing::Test {
protected:
    int current_port;
    std::unique_ptr<Reactor> reactor;
    std::thread reactor_thread;

    void SetUp() override {
        current_port = next_test_port++;
        // מאתחלים את הריאקטור עם 2 ת'רדים לעיבוד
        reactor = std::make_unique<Reactor>(2, current_port);
    }

    void TearDown() override {
        if (reactor) {
            reactor->shutdown(); // איתות ללולאה להסתיים
            if (reactor_thread.joinable()) {
                reactor_thread.join(); // המתנה לסיום מסודר של ת'רד השרת
            }
        }
    }

    // פונקציית עזר להפעלת השרת ברקע
    void start_reactor_in_background() {
        reactor_thread = std::thread([this]() {
            reactor->start();
        });
        // נותנים לשרת 50 אלפיות השנייה לעלות ולהתחיל להאזין
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // פונקציית עזר שמדמה לקוח אמיתי שמתחבר לשרת
    int create_client_and_connect() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        
        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(current_port);
        inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);
        
        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock);
            return -1;
        }
        return sock;
    }
};

// ==========================================
// קבוצה 1: בדיקות אתחול חוקיות קלט
// ==========================================

TEST_F(ReactorTest, ThrowsOnInvalidArguments) {
    // בדיקה שאתחול עם מספר ת'רדים שלילי או פורט לא חוקי נכשל מיד
    EXPECT_THROW(Reactor(2, -1), std::runtime_error);
    EXPECT_THROW(Reactor(2, 70000), std::runtime_error); // פורט מעל 65535
}

// ==========================================
// קבוצה 2: ניהול מחזור חיים (Lifecycle)
// ==========================================

TEST_F(ReactorTest, StartsAndShutsDownCleanly) {
    start_reactor_in_background();
    
    // אם ה-Reactor לא היה מממש את shutdown ו-wakeup_fd כראוי, 
    // הטסט הזה היה נתקע לנצח (Deadlock) בזמן ה-TearDown כי ה-thread_join לא היה מסיים.
    SUCCEED(); 
}

// ==========================================
// קבוצה 3: חיבור לקוחות ועבודה מול Epoll
// ==========================================

TEST_F(ReactorTest, AcceptsClientConnections) {
    start_reactor_in_background();
    
    int client_fd = create_client_and_connect();
    ASSERT_GE(client_fd, 0) << "Client failed to connect to the Reactor's port.";
    
    // סגירת הלקוח
    close(client_fd);
    
    // ניתן לשרת רגע לקלוט את הניתוק ולנקות משאבים
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    SUCCEED();
}

TEST_F(ReactorTest, ProcessesIncomingDataWithoutCrashing) {
    start_reactor_in_background();
    
    int client_fd = create_client_and_connect();
    ASSERT_GE(client_fd, 0);

    // נשלח הודעה חוקית לפי הפרוטוקול שקבענו (4 בתים אורך, ואז ה-Payload)
    uint32_t len = htonl(4);
    send(client_fd, &len, 4, 0);
    send(client_fd, "PING", 4, 0);

    // ממתינים מעט כדי שה-epoll יקלוט את ה-EPOLLIN, יקרא ל-ConnectionHandler, 
    // יעביר את זה ל-ActorThreadPool, וידפיס למסך.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    close(client_fd);
    SUCCEED();
}

// ==========================================
// קבוצה 4: יציבות בעומס מרובה לקוחות
// ==========================================

TEST_F(ReactorTest, HandlesMultipleClientsSimultaneously) {
    start_reactor_in_background();
    
    const int num_clients = 20;
    std::vector<int> client_fds;

    // 20 לקוחות מתחברים בו זמנית
    for (int i = 0; i < num_clients; ++i) {
        int fd = create_client_and_connect();
        ASSERT_GE(fd, 0);
        client_fds.push_back(fd);
    }

    // כולם שולחים מידע בו זמנית
    for (int fd : client_fds) {
        uint32_t len = htonl(2);
        send(fd, &len, 4, 0);
        send(fd, "OK", 2, 0);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // כולם מתנתקים
    for (int fd : client_fds) {
        close(fd);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // אם השרת לא קרס מכל הבלגן הזה - עברנו בהצלחה!
    SUCCEED(); 
}

// ==========================================
// קבוצה 5: עמידות בפני לקוחות בעייתיים (Bad Actors)
// ==========================================

TEST_F(ReactorTest, ClientDisconnectsMidHeader) {
    start_reactor_in_background();
    int client_fd = create_client_and_connect();
    ASSERT_GE(client_fd, 0);

    // הלקוח שולח רק 2 בתים מתוך ה-4 של ה-Header (אורך ההודעה)
    uint32_t len = htonl(100);
    send(client_fd, &len, 2, 0); 
    
    // ואז מתנתק בפתאומיות
    close(client_fd);

    // ממתינים קצת כדי לוודא שה-Reactor מקבל את אירוע הניתוק (EPOLLRDHUP) 
    // ומנקה את ה-ConnectionHandler מבלי לקרוס (Segmentation Fault).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SUCCEED();
}

TEST_F(ReactorTest, ReactorDisconnectsOnOversizedPayload) {
    start_reactor_in_background();
    int client_fd = create_client_and_connect();
    ASSERT_GE(client_fd, 0);

    // הלקוח מנסה לעשות התקפת הצפת זיכרון - טוען שיש לו הודעה של 15 מגה-בייט
    uint32_t huge_len = htonl(15 * 1024 * 1024); 
    send(client_fd, &huge_len, 4, 0);

    // השרת (דרך ה-ConnectionHandler) אמור לזהות את זה ולסגור לנו את החיבור.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ננסה לקרוא מהשרת. אם הוא סגר את החיבור, recv יחזיר 0.
    char buf[10];
    ssize_t res = recv(client_fd, buf, sizeof(buf), 0);
    EXPECT_EQ(res, 0) << "Server should have closed the connection but didn't.";
    
    close(client_fd);
}

// ==========================================
// קבוצה 6: עומס חיבורים וניתוקים (Connection Churn)
// ==========================================

TEST_F(ReactorTest, RapidConnectAndDisconnectSpam) {
    start_reactor_in_background();
    
    // מדמה מצב של סריקת פורטים (Port Scanning) או המון לקוחות שמתחברים ומתנתקים מיד
    for (int i = 0; i < 100; ++i) {
        int fd = create_client_and_connect();
        ASSERT_GE(fd, 0);
        // סוגר מיד בלי לשלוח כלום
        close(fd); 
        // השהייה קטנטנה כדי לתת ל-epoll לקלוט את ה-accept ואז את ה-close
        std::this_thread::sleep_for(std::chrono::microseconds(100)); 
    }

    // אם הגענו לכאן וה-Reactor עדיין פועל (לא קרס מניהול כושל של זיכרון או פוינטרים), עברנו.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SUCCEED();
}

// ==========================================
// קבוצה 7: קיטוע חמור והודעות ענק (Extreme Fragmentation)
// ==========================================

TEST_F(ReactorTest, HandlesLargeFragmentedPayload) {
    start_reactor_in_background();
    int client_fd = create_client_and_connect();
    ASSERT_GE(client_fd, 0);

    // נשלח הודעה גדולה אבל חוקית - מגה-בייט 1
    const int payload_size = 1024 * 1024;
    uint32_t len = htonl(payload_size);
    send(client_fd, &len, 4, 0);

    std::vector<char> big_data(payload_size, 'A');
    int sent_bytes = 0;

    // אנחנו לא שולחים את זה במכה אחת. אנחנו מציפים את השרת בחתיכות של 4KB,
    // מה שיגרום ל-Epoll להתעורר עשרות פעמים לאותו לקוח ולתור האקטורים לעבוד קשה.
    while (sent_bytes < payload_size) {
        int chunk = std::min(4096, payload_size - sent_bytes);
        ssize_t res = send(client_fd, big_data.data() + sent_bytes, chunk, 0);
        if (res > 0) {
            sent_bytes += res;
        }
        // גורם לקיטוע ברשת במכוון
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    // נותנים לשרת זמן לעבד את ההודעה השלמה (המחלקה ConnectionHandler תרכיב הכל יחד)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // נוודא שהשרת לא סגר עלינו את החיבור כי הוא חשב שקרתה שגיאה
    char buf[1];
    ssize_t peek_res = recv(client_fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
    EXPECT_NE(peek_res, 0) << "Server improperly closed connection during fragmented read.";

    close(client_fd);
}