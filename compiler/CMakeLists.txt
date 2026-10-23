cmake_minimum_required(VERSION 3.25)
project(kov C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(kovc
  src/main.cpp
  src/lexer.cpp
  src/parser.cpp
  src/resolver.cpp
  src/types.cpp
  src/source.cpp
)
target_include_directories(kovc PRIVATE src)
if (MSVC)
  target_compile_options(kovc PRIVATE /W4 /EHsc)
  target_compile_definitions(kovc PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
