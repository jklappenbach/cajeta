CMAKE_MINIMUM_REQUIRED(VERSION 3.25 FATAL_ERROR)
project(cajeta VERSION 1.0.0)

cmake_policy(SET CMP0135 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
project(cajeta VERSION 1.0)
set(CMAKE_CXX_STANDARD 17)
set(Java_JAVA_EXECUTABLE "java")

include_directories(src)

add_subdirectory(src)
add_subdirectory(test)
