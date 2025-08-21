cmake_minimum_required(VERSION 3.16)
project(chat_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 默认 Release，可用 -DCMAKE_BUILD_TYPE=Debug 切换
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 严格一点的编译警告
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion)
endif()

# ---- 源文件（按你当前目录结构）----
add_executable(chat_server
    src/main.cpp
    core/server.cpp
    file/file_catalog.cpp
    http/http_server.cpp
    src/common/logger.cpp
)

# 头文件搜索：把项目根目录加入（即可 #include "common/xxx.hpp"）
target_include_directories(chat_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 线程库
find_package(Threads REQUIRED)
target_link_libraries(chat_server PRIVATE Threads::Threads)

# nlohmann/json（两种方式：有系统包就用系统包；否则走本地 third_party 头文件）
find_package(nlohmann_json QUIET)
if (nlohmann_json_FOUND)
    target_link_libraries(chat_server PRIVATE nlohmann_json::nlohmann_json)
else()
    # 如果你把单头 json.hpp 放到 third_party/nlohmann/json.hpp，则解开下面一行
    # target_include_directories(chat_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
endif()


