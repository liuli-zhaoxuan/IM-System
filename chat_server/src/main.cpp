// #include <cstdlib>
// #include <csignal>
// #include <iostream>
// #include "common/logger.hpp"
// #include "core/server.hpp"

// static EpollChatServer* g_server = nullptr;

// static void handle_signal(int sig) {
//     if (g_server) {
//         LOG_WARN("Signal {} received, shutting down...", sig);
//         g_server->stop();
//     }
// }

// int main(int argc, char** argv) {
//     (void)argc; (void)argv;

//     // TODO: 读取 config/server.conf（先用默认值占位）
//     ServerConfig cfg;
//     cfg.ip   = "0.0.0.0";
//     cfg.port = 5000;

//     // 初始化日志（后续可从配置文件读取日志等级/路径）
//     Logger::init(LogLevel::INFO);

//     EpollChatServer server(cfg);
//     g_server = &server;

//     // 信号处理，优雅退出
//     std::signal(SIGINT,  handle_signal);
//     std::signal(SIGTERM, handle_signal);

//     LOG_INFO("Chat server starting on %s:%d (mode: epoll)", cfg.ip.c_str(), cfg.port);

//     if (!server.start()) {
//         LOG_ERROR("Server start failed.");
//         return EXIT_FAILURE;
//     }

//     // 进入主循环（阻塞直到 stop())
//     server.run();

//     LOG_INFO("Server exited. Bye.");
//     return EXIT_SUCCESS;
// }

#include <csignal>
#include <atomic>
#include <thread>

#include "common/logger.hpp"
#include "common/file_bus.hpp"
#include "file/file_catalog.hpp"
#include "http/http_server.hpp"
#include "core/server.hpp"

static std::atomic_bool g_stop{false};
static EpollChatServer* g_chat = nullptr;
static HttpServer*      g_http = nullptr;

static void handle_signal(int) {
    g_stop.store(true);
    if (g_chat) g_chat->stop();
    if (g_http) g_http->stop();
}

int main() {
    // 配置
    ServerConfig chat_cfg{ "0.0.0.0", 9000 };
    const std::string http_bind = "0.0.0.0";
    const int         http_port = 9080;
    const std::string upload_root = "uploads";

    Logger::init(LogLevel::INFO);

    FileCatalog catalog(upload_root);
    if (!catalog.init()) { LOG_ERROR("FileCatalog init failed"); return 1; }

    FileBus bus;
    if (!bus.init()) { LOG_ERROR("FileBus init failed"); return 1; }

    // HTTP 线程
    HttpServer http(http_bind, http_port, bus, catalog);
    g_http = &http;
    std::thread th_http([&]{
        if (!http.start()) { LOG_ERROR("HTTP start failed"); return; }
        http.run();
    });

    // 聊天（主线程）
    EpollChatServer chat(chat_cfg, &bus, &catalog);
    g_chat = &chat;

    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (!chat.start()) {
        LOG_ERROR("Chat start failed");
        http.stop();
        if (th_http.joinable()) th_http.join();
        return 1;
    }
    chat.run();

    http.stop();
    if (th_http.joinable()) th_http.join();
    LOG_INFO("Server exited. Bye.");
    return 0;
}
