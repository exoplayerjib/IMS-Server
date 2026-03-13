# server/01-network/tests/CMakeLists.txt

# משיכת GTest ישירות מהגיטהאב של גוגל בזמן הקימפול
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
FetchContent_MakeAvailable(googletest)

# הגדרת קובץ ההרצה של הטסטים
add_executable(network_tests 
    executor_test.cpp
    actor_thread_pool_test.cpp
    connection_handler_test.cpp
    reactor_tests.cpp
    # כאן נוסיף קבצי טסטים נוספים בהמשך (למשל reactor_test.cpp)
)

# קישור הטסטים לספריית הרשת שלך ול-GTest
target_link_libraries(network_tests PRIVATE network_layer gtest_main)

# גילוי אוטומטי של הטסטים כדי שנוכל להריץ אותם עם ctest
include(GoogleTest)
gtest_discover_tests(network_tests)