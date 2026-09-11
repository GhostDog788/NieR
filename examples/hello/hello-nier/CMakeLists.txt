cmake_minimum_required(VERSION 3.20)
project(Hello LANGUAGES C)

add_executable(hello main.c hello.c)
set_target_properties(hello PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO)
