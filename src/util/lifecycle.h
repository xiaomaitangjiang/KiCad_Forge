// 应用存活判定 —— 退出的依据是**原生窗口关闭**（webview_run 返回），
// 心跳只用来兜底「前端已经崩了但窗口还开着」这一种情况。
//
// 因此心跳超时必须远大于任何一次阻塞：导入、全库分类、长事务、
// 以及 WebView 在窗口最小化/被遮挡时对定时器的节流，都不该被判成死亡。
#pragma once

#include <cstdint>

namespace kforge::util
{

/// 前端心跳兜底超时：比任何正常阻塞（导入、分类）和后台节流都长。
/// 调小它会直接把「慢操作」变成「应用自杀」。
inline constexpr int64_t kHeartbeatTimeoutMs = 120000;

/// 前端是否仍算活着。
/// window_minimized：窗口最小化/被遮挡时 WebView 会节流页面定时器，
/// 心跳稀疏属于正常现象，此时不做判死；窗口不可见但进程已崩的情况由
/// 用户关闭窗口（原生 close 事件）结束。
inline bool frontend_alive(int64_t ms_since_heartbeat, bool window_minimized)
{
    if (window_minimized)
        return true;
    return ms_since_heartbeat < kHeartbeatTimeoutMs;
}

}  // namespace kforge::util
