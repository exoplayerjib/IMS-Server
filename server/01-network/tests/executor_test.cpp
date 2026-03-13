#include <gtest/gtest.h>
#include "executor.h"
#include <atomic>
#include <thread>
#include <vector>
#include <stdexcept>
#include <chrono>

// --- קבוצה 1: בדיקות אתחול וקלט חוקי ---

TEST(ExecutorTest, ConstructorThrowsOnInvalidThreadCount) {
    // בדיקה שניסיון ליצור Executor עם 0 או מספר שלילי של ת'רדים זורק שגיאה
    EXPECT_THROW(Executor(0), std::invalid_argument);
    EXPECT_THROW(Executor(-5), std::invalid_argument);
}

TEST(ExecutorTest, ExecuteThrowsOnEmptyTask) {
    Executor executor(2);
    std::function<void()> empty_task;
    // בדיקה שניסיון לדחוף משימה ריקה זורק שגיאה
    EXPECT_THROW(executor.execute(empty_task), std::invalid_argument);
}

// --- קבוצה 2: בדיקות מצב (State) ---

TEST(ExecutorTest, ExecuteThrowsAfterShutdown) {
    Executor executor(2);
    executor.shutdown();
    // לא ניתן להוסיף משימות אחרי שקראנו ל-shutdown
    EXPECT_THROW(executor.execute([](){}), std::runtime_error);
}

// --- קבוצה 3: עמידות (Resilience) ---

TEST(ExecutorTest, ThreadSurvivesTaskException) {
    // אנו פותחים Executor עם ת'רד אחד בלבד
    Executor executor(1);
    std::atomic<bool> second_task_ran{false};

    // המשימה הראשונה זורקת שגיאה בכוונה
    executor.execute([]() {
        throw std::runtime_error("Task failed successfully");
    });

    // המשימה השנייה אמורה לרוץ בכל זאת, מה שמוכיח שהת'רד לא מת מהשגיאה
    executor.execute([&second_task_ran]() {
        second_task_ran = true;
    });

    // שימוש ב-shutdown ימתין עד שהתור יתרוקן (הצורה הנכונה לחכות בטסט)
    executor.shutdown();
    
    EXPECT_TRUE(second_task_ran.load());
}

// --- קבוצה 4: עומס ומקביליות (Concurrency & Load) ---

TEST(ExecutorTest, HighConcurrencyLoad) {
    Executor executor(4);
    std::atomic<int> counter{0};
    
    const int num_producers = 10; // 10 ת'רדים חיצוניים שדוחפים משימות
    const int tasks_per_producer = 1000; // כל אחד דוחף אלף משימות

    auto producer_func = [&]() {
        for (int i = 0; i < tasks_per_producer; ++i) {
            executor.execute([&counter]() {
                counter++; // הפעולה עצמה מוגנת כי counter הוא אטומי
            });
        }
    };

    // יצירת הת'רדים הדוחפים
    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back(producer_func);
    }

    // המתנה שכל הדוחפים יסיימו לדחוף
    for (auto& t : producers) {
        t.join();
    }

    // קריאה ל-shutdown תוודא שה-Executor מסיים את כל 10,000 המשימות לפני שהוא נסגר
    executor.shutdown(); 

    // בדיקה ששום משימה לא הלכה לאיבוד בדרך
    EXPECT_EQ(counter.load(), num_producers * tasks_per_producer);
}