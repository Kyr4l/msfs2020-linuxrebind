#pragma once
// Small thread-safe ring-buffer logger shared by all threads; the UI renders a snapshot.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace lr {

enum class LogLevel { Debug, Info, Warn, Error };

struct LogEntry {
    std::chrono::system_clock::time_point time;
    LogLevel level;
    std::string text;
};

class Log {
public:
    static Log& instance() {
        static Log log;
        return log;
    }

    void add(LogLevel level, std::string text) {
        std::lock_guard<std::mutex> lock(m_);
        entries_.push_back({std::chrono::system_clock::now(), level, std::move(text)});
        while (entries_.size() > capacity_) entries_.pop_front();
        ++serial_;
    }

    __attribute__((format(printf, 3, 4)))
    void logf(LogLevel level, const char* fmt, ...) {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        add(level, buf);
    }

    std::vector<LogEntry> snapshot() const {
        std::lock_guard<std::mutex> lock(m_);
        return {entries_.begin(), entries_.end()};
    }

    uint64_t serial() const {
        std::lock_guard<std::mutex> lock(m_);
        return serial_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_);
        entries_.clear();
        ++serial_;
    }

    bool echoToStderr = true;

private:
    mutable std::mutex m_;
    std::deque<LogEntry> entries_;
    size_t capacity_ = 3000;
    uint64_t serial_ = 0;
};

#define LR_LOG(level, ...)                                                                 \
    do {                                                                                   \
        ::lr::Log::instance().logf(level, __VA_ARGS__);                                    \
        if (::lr::Log::instance().echoToStderr) {                                          \
            fprintf(stderr, "[%s] ", ::lr::logLevelName(level));                           \
            fprintf(stderr, __VA_ARGS__);                                                  \
            fputc('\n', stderr);                                                           \
        }                                                                                  \
    } while (0)

inline const char* logLevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "?";
}

#define LOG_DEBUG(...) LR_LOG(::lr::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) LR_LOG(::lr::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) LR_LOG(::lr::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) LR_LOG(::lr::LogLevel::Error, __VA_ARGS__)

}  // namespace lr
