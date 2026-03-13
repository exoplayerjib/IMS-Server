#include <gtest/gtest.h>
#include "actor_thread_pool.h"
#include "eventhandler.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <memory>

// --- מחלקת דמה (Mock) עבור IEventHandler ---
// מאפשרת לנו לייצר "אקטורים" מזויפים עבור הטסטים מבלי לפתוח Sockets אמיתיים
class DummyActor : public IEventHandler {
private:
    int fd;
public:
    DummyActor(int fd = 0) : fd(fd) {}
    std::function<void()> handle_read() override { return nullptr; }
    void handle_write() override {}
    int get_fd() override { return fd; }
    bool is_closed() const override { return false; }
};

// ==========================================
// קבוצה 1: חריגות וקלטים לא חוקיים (Edge Cases)
// ==========================================

TEST(ActorThreadPoolTest, ThrowsOnNullptrSubmit) {
    ActorThreadPool pool(2);
    // מצפים לשגיאה כשמעבירים nullptr
    EXPECT_THROW(pool.submit(nullptr, [](){}), std::invalid_argument);
}

TEST(ActorThreadPoolTest, ThrowsOnSubmitAfterShutdown) {
    ActorThreadPool pool(2);
    auto actor = std::make_shared<DummyActor>(1);
    pool.shutdown();
    
    // מצפים שזריקת משימה אחרי shutdown תיכשל
    EXPECT_THROW(pool.submit(actor, [](){}), std::runtime_error);
}

TEST(ActorThreadPoolTest, RemoveNullptrThrows) {
    ActorThreadPool pool(2);
    // ניסיון להסיר אקטור שהוא null
    EXPECT_THROW(pool.remove_actor(nullptr), std::invalid_argument);
}

// ==========================================
// קבוצה 2: וידוא חוקיות ריצה עוקבת לאותו אקטור (Serialization)
// ==========================================

TEST(ActorThreadPoolTest, TasksForSingleActorAreSerialized) {
    // נשתמש ב-4 ת'רדים, אבל כל המשימות יהיו שייכות לאקטור *אחד* בלבד.
    // לכן, למרות שיש 4 ת'רדים פנויים, אנחנו מצפים שהמשימות ירוצו אחת אחרי השנייה.
    ActorThreadPool pool(4); 
    auto actor = std::make_shared<DummyActor>(10);
    
    std::atomic<int> concurrent_executions{0};
    std::atomic<int> completed_tasks{0};
    int num_tasks = 100;

    for (int i = 0; i < num_tasks; ++i) {
        pool.submit(actor, [&]() {
            // אם המקביליות של האקטור נכשלה - המשתנה הזה יהיה גדול מ-1
            int current = ++concurrent_executions;
            EXPECT_LE(current, 1) << "Multiple tasks for the SAME actor ran simultaneously!";
            
            // דימוי עיבוד נתונים קצר
            std::this_thread::sleep_for(std::chrono::microseconds(50)); 
            
            --concurrent_executions;
            completed_tasks++;
        });
    }

    // המתנה פעילה לסיום כל המשימות, עם מנגנון Timeout למניעת תקיעות בטסט
    auto start = std::chrono::steady_clock::now();
    while (completed_tasks.load() < num_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            FAIL() << "Timeout: Tasks did not finish in time.";
        }
    }
    
    EXPECT_EQ(completed_tasks.load(), num_tasks);
}

// ==========================================
// קבוצה 3: מקביליות מלאה לאקטורים שונים (Parallelism)
// ==========================================

TEST(ActorThreadPoolTest, TasksForDifferentActorsRunInParallel) {
    int num_threads = 4;
    ActorThreadPool pool(num_threads);
    std::vector<std::shared_ptr<DummyActor>> actors;
    
    std::atomic<int> running_tasks{0};
    std::atomic<bool> wait_flag{true};

    // ניצור 4 אקטורים שונים וניתן לכל אחד מהם משימה ארוכה שנתקעת בכוונה.
    for (int i = 0; i < num_threads; ++i) {
        auto actor = std::make_shared<DummyActor>(100 + i);
        actors.push_back(actor);
        
        pool.submit(actor, [&]() {
            running_tasks++;
            // הת'רד מחכה פה עד שהטסט הראשי מאשר לו להמשיך
            while (wait_flag.load()) {
                std::this_thread::yield();
            }
        });
    }

    // עכשיו נבדוק שבאמת 4 ת'רדים הצליחו להיכנס במקביל לאזור הריצה
    // (כי כל אחד מהם שייך לאקטור אחר, אז ה-ThreadPool היה אמור לאפשר זאת).
    auto start = std::chrono::steady_clock::now();
    while (running_tasks.load() < num_threads) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            wait_flag = false; // שחרור במקרה של כישלון למניעת Deadlock
            FAIL() << "ThreadPool did not execute different actors in parallel.";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // אם הגענו לכאן - זה אומר שכל 4 האקטורים רצים במקביל בהצלחה!
    EXPECT_EQ(running_tasks.load(), num_threads);
    wait_flag = false; // שחרור הת'רדים כדי שיוכלו לסיים
}

// ==========================================
// קבוצה 4: ניהול אקטורים (Removal)
// ==========================================

TEST(ActorThreadPoolTest, RemoveActorClearsItSafely) {
    ActorThreadPool pool(2);
    auto actor = std::make_shared<DummyActor>(99);
    
    std::atomic<bool> task_ran{false};
    
    // משימה ראשונה כדי לגרום לאקטור להירשם במערכת
    pool.submit(actor, [&]() {
        task_ran = true;
    });

    // המתנה שהיא תסתיים
    auto start = std::chrono::steady_clock::now();
    while (!task_ran.load()) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1)) FAIL();
    }

    // הסרת האקטור (אמור למחוק אותו מה-Map ומה-Ready Queue)
    EXPECT_NO_THROW(pool.remove_actor(actor.get()));
    
    // נוודא שגם אחרי המחיקה הפול מתפקד כרגיל עם אותו אקטור אם הוא חוזר (נרשם מחדש)
    std::atomic<bool> second_task_ran{false};
    pool.submit(actor, [&]() {
        second_task_ran = true;
    });
    
    pool.shutdown(); // יחכה עד שהמשימה השנייה תסתיים
    
    EXPECT_TRUE(second_task_ran.load());
}