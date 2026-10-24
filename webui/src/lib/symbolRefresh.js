/// 符号刷新：查询状态（关键词 + 目标库）由一个 ref 持有，刷新永远读“最新”值。
///
/// 这样 await 期间用户改了查询，刷新读到的也是新查询；反过来，一次已经发出的
/// 旧查询也不会在完成时把 UI 状态改回去（状态只由 UI 动作写）。

/// 创建一个稳定的刷新回调。load 就是 useSymbols().loadSymbols，
/// getQuery 必须返回 { search, targetLib } 的当前值（通常是读 ref）。
export function createSymbolRefresher(load, getQuery) {
  return function refreshSymbols() {
    const query = getQuery() || {}
    return load(query.search || '', query.targetLib || '')
  }
}

/// “All”（清空选择）：关键词是用户的输入，不能顺手清掉 —— 只同步清空目标库
/// （state 与 ref 一起），然后按最新 ref 刷新一次：请求里关键词保持不变、
/// 目标库为空。
export function selectAllQuery({ resetTargetLib, refresh }) {
  resetTargetLib('')
  return refresh()
}

/// workspace 切换的查询重置流程：
///   1) 同步清空 UI 查询与 ref —— 不能读旧 ref（旧 workspace 的查询不该带过去）；
///   2) 立刻按空查询取一次；
///   3) 配置保存完成后按“最新” ref 刷新：用户等待期间输入的 q2 / 选择的 lib2
///      必须生效，完成回调不得再用空查询覆盖它。
export async function switchWorkspaceQuery({ resetQuery, loadFresh, save, refresh }) {
  resetQuery('', '')
  await loadFresh('', '')
  await save()
  return refresh()
}
