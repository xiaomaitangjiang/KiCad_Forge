// 应用判定退出的依据是窗口关闭返回的webview_run，
// 心跳用来防止出现前端已经崩了但窗口或后台还开着的情况。
// 因此心跳超时必须远大于任何一次阻塞：导入、全库分类、长事务、
// WebView 在窗口最小化/被遮挡时对定时器的节流，不该认为前端窗口崩溃了。
#pragma once

#include <cstdint>

namespace kforge::util
{

// 前端心跳超时
// 调小可能会误判。
inline constexpr int64_t kHeartbeatTimeoutMs = 120000;

// 前端是否仍算活着。
// window_minimized：窗口最小化/被遮挡时 WebView 会节流页面定时器，
// 心跳稀疏属于正常现象；窗口不可见但进程已崩的情况由
// 用户关闭窗口（原生 close 事件）结束。
inline bool frontend_alive(int64_t ms_since_heartbeat, bool window_minimized)
{
    if (window_minimized)
    {
        return true;
    }
    return ms_since_heartbeat < kHeartbeatTimeoutMs;
}

}  // namespace kforge::util
