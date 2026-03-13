# יצירת הספריה עם כל קבצי המקור הקיימים
add_library(network_layer 
    src/actor_thread_pool.cpp
    src/connection_handler.cpp
    src/executor.cpp
    src/reactor.cpp
)

# מתן גישה לתיקיית include גם לספריה וגם למי שישתמש בה
target_include_directories(network_layer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# הפעלת תמיכה בטסטים והוספת תיקיית הבדיקות (שניצור תיכף)
enable_testing()
add_subdirectory(tests)