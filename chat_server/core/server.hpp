// #pragma once
// #include <string>
// #include <unordered_map>
// #include <atomic>
// #include <netinet/in.h>
// #include "common/noncopyable.hpp"
// #include <nlohmann/json.hpp>
// #include <mutex>
// #include <deque>
// #include "file/file_catalog.hpp"
// #include "common/file_bus.hpp"

// // 可按需放到 config/server.conf
// static const size_t MAX_USERNAME_LEN = 32;
// static const size_t MAX_PASSWORD_LEN = 64;

// struct ServerConfig
// {
//     std::string ip;
//     int port = 0;
// };

// struct ClientInfo
// {
//     uint64_t user_id;      // 客户端唯一 ID
//     std::string user_name; // 昵称
//     std::string password;  // 密码
//     sockaddr_in user_addr; // 客户端地址
//     time_t last_active;
//     std::string recv_buffer;       // 接收缓冲区
//     std::string send_buffer;       // 待发送缓冲（避免一次 send 发不完）
//     bool is_authenticated = false; // 是否已认证
//     bool is_registered = false;    // 是否已注册

//     // === 背压：作为发送者时使用 ===
//     std::deque<std::string> relay_q; // 暂存未能广播出去的完整帧
//     size_t relay_bytes = 0;          // 队列内累计字节
//     bool rx_paused = false;          // 因下游背压已暂停读取
// };

// // ======== 大文件传输相关 ========
// // ==== Binary frame protocol (CFS1) ====
// #pragma pack(push, 1)
// struct FrameHeader
// {
//     uint32_t magic;    // "CFS1" = 0x43465331 (big-endian on the wire)
//     uint16_t type;     // 1=CHAT(JSON) 2=ONLINE(JSON) 10=FILE_BEGIN(JSON) 11=FILE_CHUNK(BIN) 12=FILE_END(JSON)
//     uint16_t flags;    // reserved
//     uint32_t length;   // payload length in bytes (big-endian)
//     uint32_t reserved; // reserved
// };
// #pragma pack(pop)

// enum : uint16_t
// {
//     FT_CHAT = 1,
//     FT_ONLINE = 2,
//     FT_FILE_BEGIN = 10,
//     FT_FILE_CHUNK = 11,
//     FT_FILE_END = 12,
// };

// // NEW: 处理结果（三态）
// enum HandleResult : uint8_t {
//     HR_OK = 0,          // 正常处理并已转发
//     HR_BACKPRESSURE = 1,// 已入队并暂停读取（上层必须立刻停止 parse）
//     HR_BAD = 2          // 非法/拒绝
// };

// static inline uint32_t be32_(uint32_t v) { return htonl(v); }
// static inline uint16_t be16_(uint16_t v) { return htons(v); }
// static inline uint64_t be64_(uint64_t v)
// {
//     uint32_t hi = htonl(uint32_t(v >> 32));
//     uint32_t lo = htonl(uint32_t(v & 0xffffffff));
//     return (uint64_t(hi) << 32) | lo;
// }
// static inline uint64_t from_be64_(uint64_t v)
// {
//     uint32_t hi = ntohl(uint32_t(v >> 32));
//     uint32_t lo = ntohl(uint32_t(v & 0xffffffff));
//     return (uint64_t(hi) << 32) | lo;
// }

// static constexpr uint32_t CFS1_MAGIC = 0x43465331; // "CFS1"

// // 服务器侧限额（按需调整）
// static constexpr size_t MAX_FRAME_PAYLOAD = 1 * 1024 * 1024;             // 单帧最大1MB
// static constexpr size_t MAX_FILE_SIZE_BYTES = 2ull * 1024 * 1024 * 1024; // 单文件最大2GB

// // ======== 大文件传输相关(结束) ========


// class EpollChatServer : NonCopyable
// {
// public:
//     explicit EpollChatServer(const ServerConfig &cfg);
//     ~EpollChatServer();

//     bool start(); // socket/bind/listen + epoll 初始化
//     void run();   // epoll 主循环（只处理接入/断开）
//     void stop();  // 请求退出

//     // ★ 新增注入
//     void setFileCatalog(FileCatalog* c) { catalog_ = c; }
//     void setFileBus(FileBus* b) { bus_ = b; }

// private:
//     // ---初始化/工具---
//     bool setup_listen_socket_();
//     bool setup_epoll_();
//     static bool set_nonblock_(int fd);

//     // --- 事件分发 ---
//     void handle_accept_();
//     void handle_events_(int fd, uint32_t ev); // 暂时只处理断开
//     void handle_write_(int fd);               // 处理发送缓冲区
//     void close_client_(int fd, const char *reason);

//     // --- 发送辅助 ---
//     void enqueue_send_(int fd, const std::string &data);           // 添加到发送缓冲区
//     void broadcast_(const std::string &line, int exclude_fd = -1); // 广播消息给所有客户端，除非指定排除的 fd

//     // --- 业务分发 ---
//     void handleClientMessage(int fd, const std::string &msg);
//     bool handle_register_(int fd, const std::string &username, const std::string &password);
//     bool handle_login_(int fd, const std::string &username, const std::string &password);
//     bool handle_chat_(int fd, const std::string &msg);
//     bool handle_online_list_(int fd);
//     bool handle_filesend_(int fd, const nlohmann::json &j);
//     void sendResponse(int fd, const std::string &response);
//     void sendErrorResponse(int fd, const std::string &reason);

//     ServerConfig cfg_;
//     int listen_fd_ = -1;
//     int epoll_fd_ = -1; // epoll 文件描述符
//     bool running_ = false;

//     // 运行参数
//     int backlog_ = 512;
//     int max_events_ = 1024;
//     int ep_timeout_ = 1000;        // ms；stop() 时最多等 1 秒退出
//     //size_t max_sendbuf_ = 1 << 20; // ✱ 每连接发送缓冲上限（1MB，背压）
//     size_t max_sendbuf_ = 64 * 1024 * 1024; // 8MB
//     // 限额（按需调整）——为了防止一个巨大的 Base64 把所有人 send_buffer 撑爆
//     size_t max_file_bytes_ = 4 * 1024 * 1024; // 原始文件最大 4MB
//     size_t max_file_b64_ = 6 * 1024 * 1024;   // Base64 文本最大 6MB（约 4/3 倍）

//     // 限额（防滥用；按需调整，用于文件并发传输）
//     size_t max_chunk_raw_ = 128 * 1024;        // 单片最大128KB
//     size_t max_chunk_b64_ = 200 * 1024;        // 单片Base64最大约~ 4/3
//     size_t max_file_size_ = 512 * 1024 * 1024; // 单文件最大512MB（可调）
//     // —— 接收/发送方向的安全阈值 —— 
//     //size_t max_recvbuf_ = 4 * 1024 * 1024;   // 每连接接收缓冲上限（4MB）
//     size_t max_recvbuf_ = 16 * 1024 * 1024; // 16MB
//     bool   drop_slow_client_frame_ = true;   // 慢客户端掉帧而非立刻断开

//     // 状态
//     std::atomic<uint64_t> online_count_{0};
//     std::atomic<uint64_t> next_id_{1}; // 用户ID自增

//     // 数据
//     std::unordered_map<int, ClientInfo> clients_info_;          // fd -> 客户端信息映射
//     std::unordered_map<std::string, ClientInfo> user_datebase_; // 用户名到用户信息的映射(用于快速查找)
//     std::unordered_map<uint64_t, std::string> user_id_to_name_; // 用户ID到昵称的映射(用于ID查找)
//     std::mutex db_mutex_;                                       // 仍然需要保护用户数据库

//     FileCatalog* catalog_ = nullptr;
//     FileBus*     bus_     = nullptr;
//     int          event_fd_= -1;

// };

#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <netinet/in.h>
#include "common/noncopyable.hpp"
#include "common/file_bus.hpp"
#include "file/file_catalog.hpp"

struct ServerConfig {
    std::string ip;
    int port = 0;
};

struct ClientInfo {
    uint64_t user_id = 0;
    std::string user_name;
    std::string password;
    sockaddr_in user_addr{};
    time_t last_active = 0;
    std::string recv_buffer;
    std::string send_buffer;
    bool is_authenticated = false;
    bool is_registered = false;
};

class EpollChatServer : NonCopyable {
public:
    explicit EpollChatServer(const ServerConfig &cfg,
                             FileBus* bus,
                             FileCatalog* catalog);
    ~EpollChatServer();

    bool start();
    void run();
    void stop();

private:
    // 初始化/工具
    bool setup_listen_socket_();
    bool setup_epoll_();
    static bool set_nonblock_(int fd);

    // 事件分发
    void handle_accept_();
    void handle_events_(int fd, uint32_t ev);
    void handle_write_(int fd);
    void close_client_(int fd, const char *reason);

    // 发送辅助
    void enqueue_send_(int fd, const std::string &data);
    void broadcast_(const std::string &line, int exclude_fd = -1);

    // 业务分发
    void handleClientMessage(int fd, const std::string &msg);
    bool handle_register_(int fd, const std::string &username, const std::string &password);
    bool handle_login_(int fd, const std::string &username, const std::string &password);
    bool handle_chat_(int fd, const std::string &msg);
    bool handle_online_list_(int fd);

    // 响应
    void sendResponse(int fd, const std::string &response);
    void sendErrorResponse(int fd, const std::string &reason);

    ServerConfig cfg_;
    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    bool running_ = false;

    // 依赖注入
    FileBus* bus_ = nullptr;
    FileCatalog* catalog_ = nullptr;

    // 运行参数
    int backlog_ = 512;
    int max_events_ = 1024;
    int ep_timeout_ = 1000;
    size_t max_sendbuf_ = 16 * 1024 * 1024;

    // 状态
    std::atomic<uint64_t> online_count_{0};
    std::atomic<uint64_t> next_id_{1};

    // 数据
    std::unordered_map<int, ClientInfo> clients_info_;
    std::unordered_map<std::string, ClientInfo> user_datebase_;
    std::unordered_map<uint64_t, std::string> user_id_to_name_;
};
