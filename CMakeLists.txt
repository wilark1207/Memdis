cmake_minimum_required(VERSION 3.10)
project(MemdisServer)

# Enforce modern C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Create the executable
add_executable(memdis-server main.cpp)
