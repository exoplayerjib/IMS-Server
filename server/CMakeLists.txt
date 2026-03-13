# add subdirectories for each layer of the server
add_subdirectory(01-network)
# add_subdirectory(02-security)
# add_subdirectory(03-parsing)
# add_subdirectory(04-dispatch)
# add_subdirectory(05-business)
# add_subdirectory(06-data)

# define the server executable and link it to the layers
# add_executable(server_app src/main.cpp)

# 3. מחברים את השרת לשכבות שיצרנו (כך הוא גם מקבל את נתיבי ה-include שלהן)
# target_link_libraries(server_app PRIVATE serviceLayer bussinesLayer)