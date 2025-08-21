#include "common/logger.hpp"
#include <mutex>
#include <array>
#include <chrono>
#include <ctime>
#include <cstring>
#include <thread>
#include <sys/syscall.h>
#include <unistd.h>

namespace
{

    // 全局状态
    static std::mutex g_mu;
    static LogLevel g_level = LogLevel::INFO;
    static FILE *g_out = stdout;
    static bool g_color = true;

    // 取线程ID
    inline unsigned long get_tid()
    {
        return static_cast<unsigned long>(gettid());
    }

    // 取文件名
    inline const char *basename2(const char *path)
    {
        if (!path)
            return "";
        const char *p = std::strrchr(path, '/');
        return p ? (p + 1) : path;
    }

    // 格式化时间到 yyyy-mm-dd HH:MM:SS.mmm
    inline void format_timestamp(char *buf, size_t n)
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto secs = time_point_cast<seconds>(now);
        auto ms = duration_cast<milliseconds>(now - secs).count();

        std::time_t t = system_clock::to_time_t(secs);
        std::tm tm_buf;
        localtime_r(&t, &tm_buf);
        std::snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<long>(ms));
    }

} // namespace

void Logger::init(LogLevel lvl, FILE *out, bool enable_color)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_level = lvl;
    g_out = out ? out : stdout;
    g_color = enable_color;
}

void Logger::set_level(LogLevel lvl)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_level = lvl;
}

LogLevel Logger::level()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_level;
}

void Logger::set_output(FILE *out)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_out = out ? out : stdout;
}

void Logger::set_color(bool enable)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_color = enable;
}

const char *Logger::level_str(LogLevel lvl)
{
    switch (lvl)
    {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARN:
        return "WARN";
    case LogLevel::ERROR:
        return "ERROR";
    }
    return "UNKNOWN";
}

const char *Logger::level_color(LogLevel lvl)
{
    if (!g_color)
        return "";

    switch (lvl)
    {
    case LogLevel::DEBUG:
        return "\033[36m"; // Cyan
    case LogLevel::INFO:
        return "\033[32m"; // Green
    case LogLevel::WARN:
        return "\033[33m"; // Yellow
    case LogLevel::ERROR:
        return "\033[31m"; // Red
    }
    return "";
}

void Logger::log(LogLevel lvl, const char *file, int line, const char *fmt, ...)
{
    if (static_cast<int>(lvl) < static_cast<int>(g_level))
        return;

    std::lock_guard<std::mutex> lk(g_mu);

    // 统一拼接到栈缓冲，减少多线程交叉
    std::array<char, 2048> buf{};
    size_t pos = 0;

    // 1) 时间戳
    char ts[64];
    format_timestamp(ts, sizeof(ts));
    pos += std::snprintf(buf.data() + pos, buf.size() - pos, "[%s]", ts);

    // 2) 等级 + 颜色
    const char *c = level_color(lvl);
    const char *r = g_color ? "\033[0m" : "";
    pos += std::snprintf(buf.data() + pos, buf.size() - pos, "%s[%s]%s", c, level_str(lvl), r);

    // 3) 线程号
    pos += std::snprintf(buf.data() + pos, buf.size() - pos, "[tid:%lu]", get_tid());

    // 4) 源文件:行号
    pos += std::snprintf(buf.data() + pos, buf.size() - pos, "[%s:%d] ", basename2(file), line);

    // 5) 正文（printf 风格）
    va_list ap;
    va_start(ap, fmt);
    pos += std::vsnprintf(buf.data() + pos, buf.size() - pos, fmt, ap);
    va_end(ap);

    // 6) 结尾换行（保证每条一行）
    if (pos < buf.size() - 1)
    {
        buf[pos++] = '\n';
        buf[pos] = '\0';
    }
    else
    {
        // 缓冲不够也强行结尾
        buf[buf.size() - 2] = '\n';
        buf[buf.size() - 1] = '\0';
    }

    std::fwrite(buf.data(), 1, std::strlen(buf.data()), g_out);
    std::fflush(g_out); // 需要实时可见时打开；追求性能可去掉
}

// void Logger::vlog(LogLevel lvl, std::string_view file, int line, const char* fmt, va_list ap) {
//     if (static_cast<int>(lvl) < static_cast<int>(g_level)) return;

//     std::lock_guard<std::mutex> lk(g_mu);

//     std::array<char, 2048> buf{};
//     size_t pos = 0;

//     char ts[64];
//     format_timestamp(ts, sizeof(ts));
//     pos += std::snprintf(buf.data() + pos, buf.size() - pos, "[%s]", ts);

//     const char* c = level_color(lvl);
//     const char* r = g_color ? "\033[0m" : "";
//     pos += std::snprintf(buf.data() + pos, buf.size() - pos, "%s[%s]%s", c, level_str(lvl), r);

//     pos += std::snprintf(buf.data() + pos, buf.size() - pos, "[tid:%lu]", get_tid());
//     pos += std::snprintf(buf.data() + pos, buf.size() - pos, "[%s:%d] ",
//                          basename(file.data()), line);

//     pos += std::vsnprintf(buf.data() + pos, buf.size() - pos, fmt, ap);

//     if (pos < buf.size() - 1) {
//         buf[pos++] = '\n';
//         buf[pos] = '\0';
//     } else {
//         buf[buf.size() - 2] = '\n';
//         buf[buf.size() - 1] = '\0';
//     }

//     std::fwrite(buf.data(), 1, std::strlen(buf.data()), g_out);
//     std::fflush(g_out);
// }