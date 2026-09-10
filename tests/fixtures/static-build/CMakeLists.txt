cmake_minimum_required(VERSION 3.20)
project(NierStaticFixture C)
add_library(duplicate STATIC one/member.c two/member.c)
