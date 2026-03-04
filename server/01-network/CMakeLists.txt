# יוצרים ספרייה בשם network_layer
# (שים פה את כל קבצי ה-cpp שיש לך בתיקיית src של השכבה הזו)
add_library(network_layer src/network.cpp)

# נותנים גישה לתיקיית ה-include.
# PUBLIC אומר: גם השכבה הזו, וגם מי שישתמש בה, יכירו את התיקייה הזו.
target_include_directories(network_layer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# *אופציונלי*: אם שכבת השירות שלך קוראת לקוד משכבת העסקים, צריך לחבר ביניהן:
target_link_libraries(network_layer PUBLIC security_layer)