# A title built through cmake/xsdk.cmake, to check the toolchain file links titles as the Makefile does.
cmake_minimum_required(VERSION 3.25)
project(cmake_check CXX)

find_package(Threads REQUIRED)

add_executable(cmake_check cmake_check.cpp)
target_compile_features(cmake_check PRIVATE cxx_std_23)
target_compile_options(cmake_check PRIVATE -Wall -Wextra -Werror)
target_link_libraries(cmake_check PRIVATE Threads::Threads)
xsdk_add_xex(cmake_check)
