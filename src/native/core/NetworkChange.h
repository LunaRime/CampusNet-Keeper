#pragma once
#include <functional>
#include <memory>

namespace cnk {

// 监听 Windows 的地址变更通知（NotifyAddrChange）。
// 在独立线程上阻塞等待，事件到来时回调。
class NetworkChangeWatcher {
public:
    NetworkChangeWatcher();
    ~NetworkChangeWatcher();
    NetworkChangeWatcher(const NetworkChangeWatcher&) = delete;
    NetworkChangeWatcher& operator=(const NetworkChangeWatcher&) = delete;

    // 回调在工作线程上执行，实现方需保证线程安全（Scheduler 只写原子标志，安全）
    bool Start(std::function<void()> on_change);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cnk
