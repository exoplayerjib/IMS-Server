cmake_minimum_required(VERSION 3.15)
project(IMS-Server CXX)

# הגדרת סטנדרט C++ לכל הפרויקט
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic )
# קריאה לתיקיות הראשיות - CMake ילך לכל אחת מהן ויחפש שם CMakeLists.txt
add_subdirectory(server)
#add_subdirectory(client)