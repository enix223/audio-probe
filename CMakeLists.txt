cmake_minimum_required(VERSION 3.16)
project(audio-probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(APPLE)
  message(FATAL_ERROR "macOS host build is not implemented yet; use build/nix on Linux")
else()
  add_subdirectory(build/nix)
endif()
