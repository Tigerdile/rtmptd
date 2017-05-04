add_executable(rtmptd main.cpp)
target_link_libraries(rtmptd ${CIVET_LIBRARY} m rt dl pthread)
