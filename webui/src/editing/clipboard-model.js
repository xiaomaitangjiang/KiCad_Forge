import { MAX_CLIPBOARD_BYTES, MAX_CLIPBOARD_ITEMS, checkCapacity } from './limits.js'

/**
 * 对象剪贴板：带应用标识/版本/种类的结构化 envelope，再加一段人可读摘要。
 *
 * WebView2/Chromium 不接受把自定义 MIME 写进系统剪贴板，所以这里用「明确
 * 前缀 + JSON」的纯文本形式；读回时前缀不对就不是我们的数据，按格式失败
 * 处理，绝不把任意文本当符号内容。
 */
export const CLIPBOARD_PREFIX = 'KFORGE-SYMBOLS-V1\n'
export const CLIPBOARD_KIND = 'kforge.symbols'
export const CLIPBOARD_VERSION = 1

export class ClipboardError extends Error {
  constructor(code, message, cause, details) {
    super(message)
    this.name = 'ClipboardError'
    this.code = code
    this.cause = cause
    if (details) Object.assign(this, details)
  }
}

export function buildEnvelope(items, { summary = '', createdAt = new Date().toISOString() } = {}) {
  if (!Array.isArray(items) || items.length === 0) {
    throw new ClipboardError('clipboardEmpty', 'nothing to copy')
  }
  if (items.length > MAX_CLIPBOARD_ITEMS) {
    throw new ClipboardError('clipboardTooLarge', 'too many items', undefined, { limit: 'items', max: MAX_CLIPBOARD_ITEMS })
  }
  return {
    app: 'KiCad_Forge',
    kind: CLIPBOARD_KIND,
    version: CLIPBOARD_VERSION,
    created_at: createdAt,
    summary: String(summary || ''),
    items: items.map(item => ({
      uuid: String(item.uuid || ''),
      kf_id: String(item.kf_id || ''),
      name: String(item.name || ''),
      library_id: String(item.library_id || ''),
      library_name: String(item.library_name || ''),
      source_path: String(item.source_path || ''),
      source_digest: String(item.source_digest || ''),
      dependency: Boolean(item.dependency),
      sexpr: String(item.sexpr || ''),
    })),
  }
}

export function serializeEnvelope(envelope) {
  return CLIPBOARD_PREFIX + JSON.stringify(envelope)
}

export function utf8Bytes(text) {
  return new TextEncoder().encode(String(text)).length
}

/** 按条目数与 UTF-8 字节数校验；超限返回 'items' | 'bytes' | null。 */
export function budgetViolation(envelope) {
  const items = Array.isArray(envelope?.items) ? envelope.items.length : 0
  const text = serializeEnvelope(envelope)
  return checkCapacity(items, utf8Bytes(text), MAX_CLIPBOARD_ITEMS, MAX_CLIPBOARD_BYTES)
}

export function parseEnvelope(text) {
  if (typeof text !== 'string') return null
  // Windows 系统剪贴板会把行尾规范成 CRLF；只接受前缀那一行的两种行尾，
  // JSON/sexpr 内容保持原样，避免改动符号文本。
  let body = null
  if (text.startsWith(CLIPBOARD_PREFIX)) body = text.slice(CLIPBOARD_PREFIX.length)
  else if (text.startsWith(CLIPBOARD_PREFIX.replace('\n', '\r\n'))) body = text.slice(CLIPBOARD_PREFIX.length + 1)
  if (body === null) return null
  let payload
  try {
    payload = JSON.parse(body)
  } catch (cause) {
    throw new ClipboardError('clipboardFormat', 'malformed symbol clipboard payload', cause)
  }
  if (!payload || typeof payload !== 'object') {
    throw new ClipboardError('clipboardFormat', 'symbol clipboard payload is not an object')
  }
  if (payload.kind !== CLIPBOARD_KIND || payload.version !== CLIPBOARD_VERSION) {
    throw new ClipboardError('clipboardFormat', 'unsupported symbol clipboard version')
  }
  if (!Array.isArray(payload.items) || payload.items.length === 0) {
    throw new ClipboardError('clipboardFormat', 'symbol clipboard payload has no items')
  }
  return payload
}

/** 写系统剪贴板；失败区分「不可用」与「被拒绝」，供界面分别提示。 */
export async function writeClipboardText(text) {
  if (utf8Bytes(text) > MAX_CLIPBOARD_BYTES) {
    throw new ClipboardError('clipboardTooLarge', 'clipboard payload exceeds byte budget', undefined, { limit: 'bytes', max: MAX_CLIPBOARD_BYTES })
  }
  let clipboard
  try {
    clipboard = globalThis.navigator?.clipboard
  } catch (cause) {
    throw new ClipboardError('clipboardUnavailable', 'clipboard API unavailable', cause)
  }
  if (!clipboard || typeof clipboard.writeText !== 'function') {
    throw new ClipboardError('clipboardUnavailable', 'clipboard API unavailable')
  }
  try {
    await clipboard.writeText(text)
  } catch (cause) {
    throw new ClipboardError('clipboardDenied', 'clipboard write was denied', cause)
  }
}

export async function readClipboardText() {
  let clipboard
  try {
    clipboard = globalThis.navigator?.clipboard
  } catch (cause) {
    throw new ClipboardError('clipboardUnavailable', 'clipboard API unavailable', cause)
  }
  if (!clipboard || typeof clipboard.readText !== 'function') {
    throw new ClipboardError('clipboardUnavailable', 'clipboard API unavailable')
  }
  try {
    return await clipboard.readText()
  } catch (cause) {
    throw new ClipboardError('clipboardDenied', 'clipboard read was denied', cause)
  }
}

/** 复制：校验预算、写剪贴板；返回写下的 envelope 供调用方报告数量。 */
export async function copyEnvelope(envelope) {
  const violation = budgetViolation(envelope)
  if (violation === 'items') {
    throw new ClipboardError('clipboardTooLarge', 'too many items', undefined, { limit: 'items', max: MAX_CLIPBOARD_ITEMS })
  }
  if (violation === 'bytes') {
    throw new ClipboardError('clipboardTooLarge', 'clipboard payload exceeds byte budget', undefined, { limit: 'bytes', max: MAX_CLIPBOARD_BYTES })
  }
  await writeClipboardText(serializeEnvelope(envelope))
  return envelope
}
