#include "sys/SingleInstance.h"

namespace cnk {

namespace {

constexpr const wchar_t* kDefaultMutexName =
    L"Local\\CampusNetKeeper.SingleInstance";

}  // namespace

SingleInstance::SingleInstance(const wchar_t* mutex_name)
    : name_(mutex_name ? mutex_name : kDefaultMutexName) {}

SingleInstance::~SingleInstance() { Release(); }

bool SingleInstance::Acquire() {
    if (handle_) return true;
    HANDLE h = CreateMutexW(nullptr, TRUE, name_.c_str());
    if (!h) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return false;
    }
    handle_ = h;
    return true;
}

void SingleInstance::Release() {
    if (!handle_) return;
    CloseHandle((HANDLE)handle_);
    handle_ = nullptr;
}

bool SingleInstance::NotifyExistingInstance() {
    HWND hwnd = FindWindowW(kHiddenWindowClass, nullptr);
    if (!hwnd) return false;
    return PostMessageW(hwnd, kMsgShowUiRequest, 0, 0) != FALSE;
}

bool SingleInstance::ConsumeUiRequest() {
    // 由任务 17 的托盘消息循环在处理 kMsgShowUiRequest 时消费；
    // 本任务先提供发送端与互斥保证，此处为占位实现。
    return false;
}

}  // namespace cnk
