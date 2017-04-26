cmake_minimum_required (VERSION 3.1)
project (rtmptd CXX)

# C++ 11
set (CMAKE_CXX_STANDARD 11)

# Version
set (rtmptd_VERSION_MAJOR 1)
set (rtmptd_VERSION_MINOR 0)

# CFlags ... find civetweb, boost, tbb
set (CMAKE_CXX_FLAGS "-I../../civetweb-1.9.1/include -g")

# Linker flags ... civetweb + tbb
set (CMAKE_EXE_LINKER_FLAGS "-L../../civetweb-1.9.1 -lcivetweb -ltbb")

# Includes
include_directories(src)
add_subdirectory(src)

