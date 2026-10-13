#include "core/NetworkChange.h"
#include "core/Log.h"
#include <winsock2.h>
#include <windows.h>  // 必须排在 winsock2.h 之后
#include <iphlpapi.h>
#include <atomic>
#include <chrono>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")

namespace cnk {

struct NetworkChangeWatcher::Impl {
    HANDLE stop_event = nullptr;
    HANDLE notify_event = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    std::function<void()> callback;

    void Loop();
};

void NetworkChangeWatcher::Impl::Loop() {
    // NotifyAddrChange 会阻塞，直到地址发生变化或句柄被关闭。
    // 为支持停止，用请求队列方式：先调一次，收到后重新挂起。
    while (running.load()) {
        OVERLAPPED ov{};
        ov.hEvent = notify_event;
        DWORD rc = NotifyAddrChange(nullptr, &ov);

        if (rc == ERROR_IO_PENDING) {
            HANDLE waits[2] = {stop_event, notify_event};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {
                // 停止请求：必须取消挂起中的地址变更请求，并等待其完成信号
                // 后再退出线程——否则内核仍持有指向本栈帧 OVERLAPPED 与事件
                // 句柄的请求，日后地址变化会写入已释放内存/向已关闭句柄发信号。
                // 带超时兜底防罕见情况下取消失败导致的挂死。
                CancelIPChangeNotify(&ov);
                WaitForSingleObject(notify_event, 2000);
                break;
            }
            if (w != WAIT_OBJECT_0 + 1) break;         // 异常
            // notify_event 是自动重置事件，等待返回时已复位，直接重挂即可
        } else {
            // 极少数情况下同步返回（地址变化已在挂起前发生）；
            // 短暂退避避免忙等
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!running.load()) break;
        }

        if (!running.load()) break;
        if (callback) {
            callback();
        }
    }
}

NetworkChangeWatcher::NetworkChangeWatcher() : impl_(new Impl) {}
NetworkChangeWatcher::~NetworkChangeWatcher() { Stop(); }

bool NetworkChangeWatcher::Start(std::function<void()> on_change) {
    if (impl_->running.load()) return true;

    // stop_event 手动重置（只等一次）；notify_event 自动重置
    //（系统完成 OVERLAPPED 时置位，等待者被唤醒后自动复位，防忙循环）
    impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->notify_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->stop_event || !impl_->notify_event) {
        LogError("创建网络监听的等待事件失败");
        Stop();
        return false;
    }

    impl_->callback = std::move(on_change);
    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->Loop(); });
    LogInfo("已启动网络地址变更监听");
    return true;
}

void NetworkChangeWatcher::Stop() {
    if (!impl_) return;
    if (impl_->running.exchange(false)) {
        if (impl_->stop_event) SetEvent(impl_->stop_event);
        if (impl_->notify_event) SetEvent(impl_->notify_event);
        if (impl_->worker.joinable()) impl_->worker.join();
    }
    if (impl_->stop_event) { CloseHandle(impl_->stop_event); impl_->stop_event = nullptr; }
    if (impl_->notify_event) { CloseHandle(impl_->notify_event); impl_->notify_event = nullptr; }
}

}  // namespace cnk
