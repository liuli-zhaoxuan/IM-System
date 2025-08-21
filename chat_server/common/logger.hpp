#pragma once
#include <cstdio>
#include <cstdarg>

enum class LogLevel
{
    DEBUG = 0,
    INFO,
    WARN,
    ERROR
};

class Logger
{
public:
    // 初始化：默认输出到stdout，等级 INFO
    static void init(LogLevel lvl = LogLevel::INFO, FILE* out = stdout, bool enable_color = true);

    static void set_level(LogLevel lvl);
    static LogLevel level();
    
    static void set_output(FILE* out);  // 设置输出流,可以切换到文字
    static void set_color(bool enable);

    // printf 风格的日志函数
    static void log(LogLevel lvl, const char* file, int line, const char* fmt, ...);

    // vpirntf 风格的日志函数
    // static void vlog(LogLevel lvl, std::string_view file, int line, const char* fmt, va_list args);

private:
    static const char* level_str(LogLevel lvl);
    static const char* level_color(LogLevel lvl);
};

// 编写宏，自动带上文件名和行号
// 使用 printf 风格的日志函数, 如：%s，%d，%zu ...
#define LOG_DEBUG(fmt, ...) Logger::log(LogLevel::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  Logger::log(LogLevel::INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Logger::log(LogLevel::WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Logger::log(LogLevel::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)