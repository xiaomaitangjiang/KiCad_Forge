// 命令不可用原因的文案键（ED-006）：菜单、右键与快捷键共用同一套。
export const REASON_KEYS = {
  noTarget: 'topbar.editNoTarget',
  noSelection: 'topbar.editNoSelection',
  noSelectionSupport: 'topbar.editNoSelectionSupport',
  readOnly: 'topbar.editReadOnly',
  noObjectSelection: 'topbar.editNoObjectSelection',
  noResults: 'topbar.editNoResults',
  objectUnsupported: 'topbar.editObjectUnsupported',
  clipboardEmpty: 'topbar.editClipboardEmpty',
  historyEmpty: 'topbar.editHistoryEmpty',
  busy: 'topbar.editBusy',
  uncertain: 'topbar.editUncertain',
  staleTarget: 'topbar.editStaleTarget',
}

export function reasonLabel(reason, t) {
  const key = REASON_KEYS[reason]
  return key && typeof t === 'function' ? t(key) : ''
}
