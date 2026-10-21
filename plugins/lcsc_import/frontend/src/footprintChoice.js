// 复用封装的选择：身份是封装 UUID (id)，库名/封装名只用来显示。
// 同名不同来源（两个 .pretty 里各有一个 SOT-23）在数量上完全可能，
// 只按名字比较会把「同库同名」的另一个封装当成同一个选择。

/// 从候选项生成复用选择：保留 id，并带上显示用的 library/name
export function reuseChoice(candidate) {
  if (!candidate) return null
  const id = candidate.id || ''
  return {
    mode: 'reuse',
    id,
    library: candidate.library || '',
    name: candidate.name || '',
  }
}

/// 当前选择是否就是这个候选项 —— 有 id 时只认 id
export function isChosen(choice, candidate) {
  if (!choice || choice.mode !== 'reuse' || !candidate) return false
  const choiceId = choice.id || choice.footprint_id || ''
  const candidateId = candidate.id || ''
  if (choiceId && candidateId) return choiceId === candidateId
  // 旧数据没有 id：退回 (库, 名) 显示字段比较
  return (choice.library || '') === (candidate.library || '') && choice.name === candidate.name
}

/// 显示文本：库名:封装名
export function choiceLabel(choice) {
  if (!choice || choice.mode !== 'reuse') return ''
  return choice.library ? `${choice.library}:${choice.name}` : (choice.name || '')
}

/// 提交体：footprint_choice 原样带上，reuse 且有 id 时显式提交 footprint_id。
/// 没有 id 的旧选择仍按 library/name 走宿主的兼容解析（唯一匹配才成功）。
/// 新建模式必须清掉调用方可能带来的旧 footprint_id：否则提交体里会留着一个
/// 已经不适用的复用 UUID，宿主一旦按它解析就绑到旧的封装上。
export function withFootprintChoice(body, choice) {
  const selected = choice || { mode: 'new' }
  const next = { ...body, footprint_choice: selected }
  if (selected.mode === 'reuse') {
    const id = selected.id || selected.footprint_id || ''
    if (id) next.footprint_id = id
    else delete next.footprint_id  // 旧选择没有 id：交给宿主唯一匹配，不伪造
  } else {
    delete next.footprint_id
  }
  return next
}
