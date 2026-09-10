cmake_minimum_required(VERSION 3.20)
project(FileIdentity C)
add_executable(hello main.c)
add_custom_command(TARGET hello POST_BUILD COMMAND "$<TARGET_FILE:hello>")
