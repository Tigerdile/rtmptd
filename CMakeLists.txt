cmake_minimum_required (VERSION 3.1)
project (rtmptd CXX)

# C++ 11
set (CMAKE_CXX_STANDARD 11)

# Version
set (rtmptd_VERSION_MAJOR 1)
set (rtmptd_VERSION_MINOR 0)

# CFlags ... find civetweb, boost
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I../../civetweb/include")

# Find Civet
find_library(CIVET_LIBRARY NAMES civetweb HINTS "../civetweb")

# Includes
include_directories(src)
add_subdirectory(src)

