cmake_minimum_required(VERSION 3.21)
project(TaskScheduleManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # so clangd / VSCode intellisense can find flags/includes

add_executable(task_schedule_manager
    main.cpp
    TaskScheduleManager.cpp
    TaskTicket.cpp
)

target_compile_options(task_schedule_manager PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(task_schedule_manager PRIVATE Threads::Threads)

# Toggled on by the "debug" preset below. Off by default so a plain
# `cmake --build .` (no preset) doesn't silently pay the sanitizer
# overhead or require a sanitizer-capable toolchain.
option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    target_compile_options(task_schedule_manager PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(task_schedule_manager PRIVATE -fsanitize=address,undefined)
endif()
