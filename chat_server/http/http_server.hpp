#pragma once
#include <string>
#include <atomic>
#include "common/file_bus.hpp"
#include "file/file_catalog.hpp"

class HttpServer {
public:
    static constexpr size_t DEFAULT_CHUNK_SIZE = 256 * 1024; // 256KB

    HttpServer(std::string bind, int port, FileBus& bus, FileCatalog& catalog);
    ~HttpServer();

    bool start();   // bind + listen
    void run();     // accept/serve loop（阻塞）
    void stop();    // 请求退出（关闭监听套接字）

private:
    int listen_fd_ = -1;
    std::string bind_;
    int port_;
    std::atomic<bool> stopping_{false};

    FileBus& bus_;
    FileCatalog& catalog_;

    bool setup_listen_();
    void serve_client_(int cfd);

    // 工具
    static std::string gen_uuid_();
    static bool parse_request_(int cfd, std::string& method, std::string& uri,
                               std::string& headers, std::string& body);
    static bool get_header_(const std::string& headers, const std::string& key, std::string& val);
    static bool parse_query_(const std::string& uri, std::string& path,
                             std::string& id, std::string& seq, std::string& name);
    static bool send_simple_(int cfd, int code, const char* status,
                             const std::string& body, const char* ctype="application/json");
    static bool send_file_(int cfd, const std::string& path, const std::string& name);
};
