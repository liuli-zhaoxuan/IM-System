#pragma once
#include <sys/eventfd.h>
#include <unistd.h>
#include <mutex>
#include <deque>
#include <string>

class FileBus {
public:
    FileBus() : efd_(-1) {}
    ~FileBus() { if (efd_ >= 0) ::close(efd_); }

    bool init() {
        efd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        return efd_ >= 0;
    }

    int fd() const { return efd_; }

    // 生产者（HTTP 线程）发布一条 JSON（或任意字符串）
    void publish(std::string msg) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(msg));
        }
        uint64_t one = 1;
        (void)::write(efd_, &one, sizeof(one)); // 非阻塞
    }

    // 消费者（TCP 线程）尝试取一条
    bool try_pop(std::string& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    // 把 eventfd 的计数清零
    void drain_eventfd() {
        uint64_t v;
        while (::read(efd_, &v, sizeof(v)) > 0) { /* drain */ }
    }

private:
    int efd_;
    std::mutex mu_;
    std::deque<std::string> q_;
};
