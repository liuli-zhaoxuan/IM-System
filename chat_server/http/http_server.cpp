#include "http/http_server.hpp"
#include "common/logger.hpp"
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

using json = nlohmann::json;

namespace
{
    int set_nonblock(int fd)
    {
        int f = fcntl(fd, F_GETFL, 0);
        return (f >= 0 && fcntl(fd, F_SETFL, f | O_NONBLOCK) >= 0) ? 0 : -1;
    }
    int set_block(int fd)
    {
        int f = fcntl(fd, F_GETFL, 0);
        return (f >= 0 && fcntl(fd, F_SETFL, f & ~O_NONBLOCK) >= 0) ? 0 : -1;
    }
    ssize_t read_n(int fd, void *buf, size_t n)
    {
        size_t got = 0;
        while (got < n)
        {
            ssize_t r = ::recv(fd, (char *)buf + got, n - got, 0);
            if (r > 0)
                got += (size_t)r;
            else if (r == 0)
                break;
            else if (errno == EINTR)
                continue;
            else
                return -1;
        }
        return (ssize_t)got;
    }
} // namespace

HttpServer::HttpServer(std::string bind, int port, FileBus &bus, FileCatalog &catalog)
    : bind_(std::move(bind)), port_(port), bus_(bus), catalog_(catalog) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::setup_listen_()
{
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
    {
        LOG_ERROR("HTTP socket() failed: %s", strerror(errno));
        return false;
    }
    int on = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, bind_.c_str(), &addr.sin_addr) <= 0)
    {
        LOG_ERROR("HTTP invalid bind ip: %s", bind_.c_str());
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_ERROR("HTTP bind() failed on %s:%d: %s", bind_.c_str(), port_, strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 512) < 0)
    {
        LOG_ERROR("HTTP listen() failed: %s", strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    set_nonblock(listen_fd_);
    return true;
}

bool HttpServer::start() { return setup_listen_(); }

void HttpServer::stop()
{
    stopping_.store(true);
    if (listen_fd_ >= 0)
    {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

static std::string url_decode(std::string s)
{
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            char h[3] = {s[i + 1], s[i + 2], 0};
            char c = (char)strtol(h, nullptr, 16);
            o.push_back(c);
            i += 2;
        }
        else if (s[i] == '+')
        {
            o.push_back(' ');
        }
        else
            o.push_back(s[i]);
    }
    return o;
}

bool HttpServer::parse_query_(const std::string &uri, std::string &path,
                              std::string &id, std::string &seq, std::string &name)
{
    path.clear();
    id.clear();
    seq.clear();
    name.clear();
    auto qpos = uri.find('?');
    path = (qpos == std::string::npos) ? uri : uri.substr(0, qpos);
    if (qpos == std::string::npos)
        return true;
    std::string qs = uri.substr(qpos + 1);
    while (!qs.empty())
    {
        auto amp = qs.find('&');
        std::string kv = (amp == std::string::npos) ? qs : qs.substr(0, amp);
        if (amp != std::string::npos)
            qs.erase(0, amp + 1);
        else
            qs.clear();
        auto eq = kv.find('=');
        std::string k = (eq == std::string::npos) ? kv : kv.substr(0, eq);
        std::string v = (eq == std::string::npos) ? "" : url_decode(kv.substr(eq + 1));
        if (k == "id")
            id = v;
        else if (k == "seq")
            seq = v;
        else if (k == "name")
            name = v;
    }
    return true;
}

bool HttpServer::get_header_(const std::string &headers, const std::string &key, std::string &val)
{
    val.clear();
    auto pos = headers.find(key);
    if (pos == std::string::npos)
        return false;
    auto end = headers.find("\r\n", pos);
    if (end == std::string::npos)
        return false;
    auto colon = headers.find(':', pos);
    if (colon == std::string::npos || colon > end)
        return false;
    size_t s = colon + 1;
    while (s < end && (headers[s] == ' ' || headers[s] == '\t'))
        ++s;
    val = headers.substr(s, end - s);
    return true;
}

bool HttpServer::parse_request_(int cfd, std::string &method, std::string &uri,
                                std::string &headers, std::string &body)
{
    method.clear(); uri.clear(); headers.clear(); body.clear();
    set_block(cfd);

    // —— 读到包含 "\r\n\r\n" 的一批数据（可多次）——
    std::string buf;
    char tmp[4096];

    for (;;) {
        // 先窥探
        ssize_t n = ::recv(cfd, tmp, sizeof(tmp), MSG_PEEK);
        if (n <= 0) return false;

        // 真正取出
        std::string chunk(tmp, tmp + n);
        if (::recv(cfd, tmp, n, 0) != n) return false; // 消费同样大小

        buf.append(chunk);
        size_t pos = buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
            headers = buf.substr(0, pos + 4);
            // —— 关键：保留已读数据里 header 之后的 body 前缀 —— //
            body = buf.substr(pos + 4);
            break;
        }

        // 保护：请求头太大
        if (buf.size() > 64 * 1024) return false;
    }

    // 解析首行
    size_t eol = headers.find("\r\n");
    if (eol == std::string::npos) return false;
    std::string first = headers.substr(0, eol);
    {
        auto p1 = first.find(' ');
        auto p2 = first.find(' ', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) return false;
        method = first.substr(0, p1);
        uri    = first.substr(p1 + 1, p2 - p1 - 1);
    }

    // Content-Length
    std::string clen_s;
    size_t clen = 0;
    if (get_header_(headers, "Content-Length", clen_s)) {
        clen = (size_t)std::strtoull(clen_s.c_str(), nullptr, 10);
        if (clen > 256ull * 1024 * 1024) return false; // 简单保护
    } else {
        // GET 没 body 可以直接返回
        if (method == "GET") return true;
        // 没有 CL 的 POST/PUT 不支持
        return false;
    }

    // —— 若首包已带 body 前缀，补读剩余的字节 —— //
    if (body.size() < clen) {
        size_t need = clen - body.size();
        std::string more;
        more.resize(need);
        ssize_t r = read_n(cfd, (void*)more.data(), need);
        if (r != (ssize_t)need) return false;
        body.append(more);
    } else if (body.size() > clen) {
        // 极少见：pipeline 情况，截断到声明长度
        body.resize(clen);
    }

    return true;
}


bool HttpServer::send_simple_(int cfd, int code, const char *status,
                              const std::string &body, const char *ctype)
{
    char hdr[512];
    int n = std::snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Content-Type: %s\r\n"
                          "Connection: close\r\n\r\n",
                          code, status, body.size(), ctype);
    if (::send(cfd, hdr, n, MSG_NOSIGNAL) < 0)
        return false;
    if (!body.empty())
        (void)::send(cfd, body.data(), body.size(), MSG_NOSIGNAL);
    return true;
}

bool HttpServer::send_file_(int cfd, const std::string &path, const std::string &name)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return send_simple_(cfd, 404, "Not Found", "NotFound", "text/plain");

    struct stat st{};
    if (::fstat(fd, &st) != 0)
    {
        ::close(fd);
        return send_simple_(cfd, 500, "Internal Error", "Err", "text/plain");
    }

    char hdr[512];
    int n = std::snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Length: %lld\r\n"
                          "Content-Type: application/octet-stream\r\n"
                          "Content-Disposition: attachment; filename=\"%s\"\r\n"
                          "Connection: close\r\n\r\n",
                          (long long)st.st_size, name.c_str());
    (void)::send(cfd, hdr, n, MSG_NOSIGNAL);

    char buf[64 * 1024];
    ssize_t r;
    while ((r = ::read(fd, buf, sizeof(buf))) > 0)
    {
        if (::send(cfd, buf, (size_t)r, MSG_NOSIGNAL) < 0)
            break;
    }
    ::close(fd);
    return true;
}

std::string HttpServer::gen_uuid_()
{
    // 简易 UUID（够 demo 用）；生产建议用真正 UUID 库
    char s[37] = {0};
    unsigned v[16];
    for (int i = 0; i < 16; ++i)
        v[i] = (unsigned)rand();
    std::snprintf(s, sizeof(s),
                  "%08x-%04x-%04x-%04x-%04x%08x",
                  v[0], v[1] & 0xffffu, v[2] & 0xffffu, v[3] & 0xffffu, v[4] & 0xffffu, v[5]);
    return s;
}

void HttpServer::serve_client_(int cfd)
{
    std::string method, uri, headers, body;
    if (!parse_request_(cfd, method, uri, headers, body))
    {
        ::close(cfd);
        return;
    }

    // 路由
    std::string path, id, seq, name;
    parse_query_(uri, path, id, seq, name);

    // 1) 健康检查
    if (method == "GET" && path == "/health")
    {
        send_simple_(cfd, 200, "OK", "OK", "text/plain");
        ::close(cfd);
        return;
    }

    // 2) 下载
    if (method == "GET" && path == "/download")
    {
        if (name.empty())
        {
            send_simple_(cfd, 400, "Bad Request", "missing name", "text/plain");
            ::close(cfd);
            return;
        }
        auto fpath = catalog_.final_path(name);
        send_file_(cfd, fpath, name);
        ::close(cfd);
        return;
    }

    // 3) 上传初始化：POST /upload/init   body: {"name":"...", "size":12345}
    if (method == "POST" && path == "/upload/init")
    {
        std::string id_new = gen_uuid_();
        auto tmp = catalog_.temp_path(id_new);

        // 预创建空的 .part
        int fd = ::open(tmp.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0)
        {
            send_simple_(cfd, 500, "Internal Error", "open temp failed", "text/plain");
            ::close(cfd);
            return;
        }
        ::close(fd);

        json resp{
            {"id", id_new},
            {"chunk_size", (int)DEFAULT_CHUNK_SIZE}};
        send_simple_(cfd, 200, "OK", resp.dump());
        ::close(cfd);
        return;
    }

    // 4) 上传分片：PUT /upload/chunk?id=...&seq=...   (Body=二进制)
    if (method == "PUT" && path == "/upload/chunk")
    {
        if (id.empty() || seq.empty())
        {
            send_simple_(cfd, 400, "Bad Request", "missing id/seq", "text/plain");
            ::close(cfd);
            return;
        }

        // 直接使用 parse_request_() 返回的 body
        const std::string& data = body;
        if (data.empty())
        {
            send_simple_(cfd, 400, "Bad Request", "empty body", "text/plain");
            ::close(cfd);
            return;
        }

        // 可选：限制单片大小（与 DEFAULT_CHUNK_SIZE 对齐）
        if (data.size() > DEFAULT_CHUNK_SIZE)
        {
            send_simple_(cfd, 413, "Payload Too Large", "chunk too large", "text/plain");
            ::close(cfd);
            return;
        }

        auto tmp = catalog_.temp_path(id);
        int fd = ::open(tmp.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd < 0)
        {
            send_simple_(cfd, 500, "Internal Error", "open temp failed", "text/plain");
            ::close(cfd);
            return;
        }

        long long iseq = std::strtoll(seq.c_str(), nullptr, 10);
        off_t off = (off_t)(iseq * (long long)DEFAULT_CHUNK_SIZE);
        ssize_t wr = ::pwrite(fd, data.data(), data.size(), off);
        ::close(fd);

        if (wr != (ssize_t)data.size())
        {
            send_simple_(cfd, 500, "Internal Error", "pwrite fail", "text/plain");
            ::close(cfd);
            return;
        }

        send_simple_(cfd, 200, "OK", "{\"ok\":true}");
        ::close(cfd);
        return;
    }

    // 5) 完成提交：POST /upload/complete    body: {"id":"..","name":"..","size":123,"from":"Alice"}
    if (method == "POST" && path == "/upload/complete")
    {
        json req;
        try
        {
            req = json::parse(body);
        }
        catch (...)
        {
            send_simple_(cfd, 400, "Bad Request", "bad json", "text/plain");
            ::close(cfd);
            return;
        }
        std::string jid = req.value("id", "");
        std::string jname = req.value("name", "");
        long long jsize = req.value("size", 0LL);
        std::string jfrom = req.value("from", "");

        if (jid.empty() || jname.empty() || jsize <= 0)
        {
            send_simple_(cfd, 400, "Bad Request", "missing fields", "text/plain");
            ::close(cfd);
            return;
        }

        auto tmp = catalog_.temp_path(jid);
        
        // 最终尺寸校验
        struct stat st{};
        if (::stat(tmp.c_str(), &st) != 0 || (long long)st.st_size != jsize) {
            send_simple_(cfd, 400, "Bad Request", "size mismatch", "text/plain");
            ::close(cfd);
            return;
        }

        auto fin = catalog_.final_path(jname);
        // 可根据 jsize 校验实际大小（略）
        ::unlink(fin.c_str());
        if (::rename(tmp.c_str(), fin.c_str()) != 0)
        {
            send_simple_(cfd, 500, "Internal Error", "rename fail", "text/plain");
            ::close(cfd);
            return;
        }

        // 向聊天侧广播文件元信息
        json meta{
            {"action", "file_meta"},
            {"from", jfrom},
            {"name", jname},
            {"size", jsize},
            {"url", std::string("/download?name=") + jname}};
        bus_.publish(meta.dump());

        send_simple_(cfd, 200, "OK", "{\"ok\":true}");
        ::close(cfd);
        return;
    }

    // 未匹配
    send_simple_(cfd, 404, "Not Found", "NotFound", "text/plain");
    ::close(cfd);
}

void HttpServer::run()
{
    LOG_INFO("HTTP server listening at http://%s:%d", bind_.c_str(), port_);
    while (!stopping_.load())
    {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int cfd = ::accept4(listen_fd_, (sockaddr *)&cli, &len, SOCK_CLOEXEC);
        if (cfd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                usleep(1000 * 10);
                continue;
            }
            if (errno == EINTR)
                continue;
            if (stopping_.load())
                break;
            LOG_WARN("HTTP accept error: %s", strerror(errno));
            continue;
        }
        serve_client_(cfd);
    }
    LOG_INFO("HTTP server stopped.");
}
