#include "core/server.hpp"
#include "common/logger.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static int set_reuseaddr(int fd)
{
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

EpollChatServer::EpollChatServer(const ServerConfig &cfg, FileBus *bus, FileCatalog *catalog)
    : cfg_(cfg), bus_(bus), catalog_(catalog) {}

EpollChatServer::~EpollChatServer()
{
    if (listen_fd_ >= 0)
        ::close(listen_fd_);
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

bool EpollChatServer::set_nonblock_(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool EpollChatServer::setup_listen_socket_()
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
    {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return false;
    }
    if (set_reuseaddr(listen_fd_) < 0)
    {
        LOG_WARN("setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
    }
    if (!set_nonblock_(listen_fd_))
    {
        LOG_ERROR("set_nonblock failed");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    if (inet_pton(AF_INET, cfg_.ip.c_str(), &addr.sin_addr) <= 0)
    {
        LOG_ERROR("invalid ip: %s", cfg_.ip.c_str());
        return false;
    }
    if (bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_ERROR("bind failed: %s", strerror(errno));
        return false;
    }
    if (listen(listen_fd_, backlog_) < 0)
    {
        LOG_ERROR("listen failed: %s", strerror(errno));
        return false;
    }
    return true;
}

bool EpollChatServer::setup_epoll_()
{
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
    {
        LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0)
    {
        LOG_ERROR("epoll_ctl ADD listen failed");
        return false;
    }

    if (bus_)
    { // 监听 bus 的 eventfd
        epoll_event bev{};
        bev.events = EPOLLIN;
        bev.data.fd = bus_->fd();
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, bus_->fd(), &bev) < 0)
        {
            LOG_ERROR("epoll_ctl ADD bus failed");
            return false;
        }
    }
    return true;
}

bool EpollChatServer::start()
{
    if (!setup_listen_socket_())
        return false;
    if (!setup_epoll_())
        return false;
    running_ = true;
    online_count_ = 0;
    LOG_INFO("Chat server listening on %s:%d", cfg_.ip.c_str(), cfg_.port);
    return true;
}

void EpollChatServer::handle_accept_()
{
    while (true)
    {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int cfd = accept4(listen_fd_, (sockaddr *)&cli, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            LOG_WARN("accept4 failed: %s", strerror(errno));
            break;
        }
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
        ev.data.fd = cfd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &ev) < 0)
        {
            LOG_WARN("epoll_ctl ADD client failed");
            ::close(cfd);
            continue;
        }

        clients_info_[cfd] = ClientInfo{};
        clients_info_[cfd].user_addr = cli;
        clients_info_[cfd].last_active = time(nullptr);

        ++online_count_;
    }
}

void EpollChatServer::enqueue_send_(int fd, const std::string &data)
{
    auto it = clients_info_.find(fd);
    if (it == clients_info_.end())
        return;
    auto &buf = it->second.send_buffer;

    if (buf.empty())
    {
        ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n == (ssize_t)data.size())
            return;
        if (n > 0)
        {
            size_t remain = data.size() - (size_t)n;
            if (remain > max_sendbuf_)
            {
                close_client_(fd, "sendbuf overflow");
                return;
            }
            buf.append(data.data() + n, remain);
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (data.size() > max_sendbuf_)
                {
                    close_client_(fd, "sendbuf overflow");
                    return;
                }
                buf.append(data);
            }
            else
            {
                close_client_(fd, "send error");
                return;
            }
        }
    }
    else
    {
        if (buf.size() + data.size() > max_sendbuf_)
        {
            close_client_(fd, "sendbuf overflow");
            return;
        }
        buf.append(data);
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
    {
        close_client_(fd, "epoll mod error");
    }
}

void EpollChatServer::handle_write_(int fd)
{
    auto it = clients_info_.find(fd);
    if (it == clients_info_.end())
        return;
    auto &buf = it->second.send_buffer;

    while (!buf.empty())
    {
        ssize_t n = ::send(fd, buf.data(), buf.size(), MSG_NOSIGNAL);
        if (n > 0)
        {
            buf.erase(0, (size_t)n);
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            close_client_(fd, "send error");
            return;
        }
    }

    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void EpollChatServer::broadcast_(const std::string &line, int exclude_fd)
{
    std::vector<int> targets;
    targets.reserve(clients_info_.size());
    for (const auto &kv : clients_info_)
    {
        int cfd = kv.first;
        const auto &c = kv.second;
        if (cfd == exclude_fd)
            continue;
        if (!c.is_authenticated)
            continue;
        targets.push_back(cfd);
    }
    for (int cfd : targets)
        enqueue_send_(cfd, line);
}

void EpollChatServer::handle_events_(int fd, uint32_t ev)
{
    // eventfd（来自 HTTP 的通知）
    if (bus_ && fd == bus_->fd())
    {
        bus_->drain_eventfd();
        std::string msg;
        while (bus_->try_pop(msg))
        {
            broadcast_(msg + "\n", -1);
        }
        return;
    }

    // 错误/断开
    if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
    {
        close_client_(fd, "client disconnected");
        return;
    }

    // 可读
    if (ev & EPOLLIN)
    {
        auto itc = clients_info_.find(fd);
        if (itc == clients_info_.end())
            return;
        auto &client = itc->second;

        char buf[4096];
        for (;;)
        {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0)
            {
                client.last_active = time(nullptr);
                client.recv_buffer.append(buf, (size_t)n);

                while (true)
                {
                    size_t nl = client.recv_buffer.find('\n');
                    if (nl == std::string::npos)
                        break;
                    std::string line = client.recv_buffer.substr(0, nl);
                    client.recv_buffer.erase(0, nl + 1);
                    if (!line.empty())
                        handleClientMessage(fd, line);
                }
                continue;
            }
            if (n == 0)
            {
                close_client_(fd, "peer closed");
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            close_client_(fd, "recv error");
            break;
        }
    }

    // 可写
    if (ev & EPOLLOUT)
        handle_write_(fd);
}

void EpollChatServer::close_client_(int fd, const char *reason)
{
    auto it = clients_info_.find(fd);
    if (it == clients_info_.end())
        return;

    LOG_INFO("[LEAVE] fd=%d name=%s, reason: %s", fd, it->second.user_name.c_str(), reason ? reason : "bye");
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    clients_info_.erase(fd);
    --online_count_;
}

void EpollChatServer::run()
{
    std::vector<epoll_event> evs((size_t)max_events_);
    while (running_)
    {
        int n = epoll_wait(epoll_fd_, evs.data(), max_events_, ep_timeout_);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR("epoll_wait failed: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i)
        {
            int fd = evs[i].data.fd;
            uint32_t ev = evs[i].events;
            if (fd == listen_fd_)
                handle_accept_();
            else
                handle_events_(fd, ev);
        }
    }

    // 清理
    for (auto &kv : clients_info_)
    {
        int fd = kv.first;
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }
    clients_info_.clear();
}

void EpollChatServer::stop() { running_ = false; }

void EpollChatServer::handleClientMessage(int fd, const std::string &msg)
{
    try
    {
        auto j = json::parse(msg);
        if (!j.contains("action"))
        {
            sendErrorResponse(fd, "missing action");
            return;
        }
        std::string action = j["action"].get<std::string>();

        if (action == "register")
        {
            if (!j.contains("username") || !j.contains("password"))
            {
                sendErrorResponse(fd, "missing fields");
                return;
            }
            handle_register_(fd, j["username"].get<std::string>(), j["password"].get<std::string>());
        }
        else if (action == "login")
        {
            if (!j.contains("username") || !j.contains("password"))
            {
                sendErrorResponse(fd, "missing fields");
                return;
            }
            if (handle_login_(fd, j["username"].get<std::string>(), j["password"].get<std::string>()))
            {
                json resp{{"status", "success"}, {"message", "Login successful"}, {"username", j["username"].get<std::string>()}};
                sendResponse(fd, resp.dump());
                handle_online_list_(fd);
            }
            else
            {
                sendErrorResponse(fd, "login failed");
            }
        }
        else if (action == "chat")
        {
            if (!clients_info_[fd].is_authenticated)
            {
                sendErrorResponse(fd, "please login");
                return;
            }
            if (!j.contains("text"))
            {
                sendErrorResponse(fd, "missing text");
                return;
            }
            handle_chat_(fd, j["text"].get<std::string>());
        }
        else if (action == "online_list")
        {
            handle_online_list_(fd);
        }
        else
        {
            sendErrorResponse(fd, "unknown action");
        }
    }
    catch (...)
    {
        sendErrorResponse(fd, "bad json");
    }
}

bool EpollChatServer::handle_register_(int fd, const std::string &username, const std::string &password)
{
    if (username.empty() || password.empty())
    {
        sendErrorResponse(fd, "empty user/pass");
        return false;
    }
    if (user_datebase_.count(username))
    {
        sendErrorResponse(fd, "user exists");
        return false;
    }

    uint64_t new_id = next_id_++;
    auto &client = clients_info_[fd];
    client.user_id = new_id;
    client.user_name = username;
    client.password = password;
    client.is_registered = true;
    client.last_active = time(nullptr);

    user_datebase_[username] = client;
    user_id_to_name_[new_id] = username;

    json resp{{"status", "success"}, {"message", "Registration successful"}, {"user_id", new_id}};
    sendResponse(fd, resp.dump());
    return true;
}

bool EpollChatServer::handle_login_(int fd, const std::string &username, const std::string &password)
{
    auto it = user_datebase_.find(username);
    if (it == user_datebase_.end())
        return false;
    if (it->second.password != password)
        return false;

    auto &client = clients_info_[fd];
    client.user_id = it->second.user_id;
    client.user_name = username;
    client.is_authenticated = true;
    client.last_active = time(nullptr);
    return true;
}

bool EpollChatServer::handle_chat_(int fd, const std::string &msg)
{
    if (msg.empty() || msg.size() > 4096)
        return false;
    auto it = clients_info_.find(fd);
    if (it == clients_info_.end())
        return false;
    const auto &c = it->second;
    std::string nick = c.user_name.empty() ? ("user" + std::to_string(c.user_id)) : c.user_name;

    json j{{"action", "chat"}, {"from", nick}, {"text", msg}};
    std::string payload = j.dump();
    payload.push_back('\n');
    broadcast_(payload, fd);
    return true;
}

bool EpollChatServer::handle_online_list_(int fd)
{
    std::unordered_set<std::string> uniq;
    for (const auto &kv : clients_info_)
    {
        const auto &c = kv.second;
        if (c.is_authenticated && !c.user_name.empty())
            uniq.insert(c.user_name);
    }
    json j;
    j["action"] = "online_info";
    j["count"] = uniq.size();
    j["users"] = json::array();
    for (const auto &n : uniq)
        j["users"].push_back(n);
    sendResponse(fd, j.dump());
    return true;
}

void EpollChatServer::sendResponse(int fd, const std::string &response) { enqueue_send_(fd, response + "\n"); }
void EpollChatServer::sendErrorResponse(int fd, const std::string &reason)
{
    json r{{"status", "fail"}, {"reason", reason}};
    sendResponse(fd, r.dump());
}
