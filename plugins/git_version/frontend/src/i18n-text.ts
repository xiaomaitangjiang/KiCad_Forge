// Pure language/text helpers shared by the model (Node tests import this
// module directly, so it must not pull in React).
export type Language = 'zh' | 'en'
export type Vars = Record<string, string | number>

function storedLanguage(): string {
  try {
    return localStorage.getItem('kf-lang') ?? ''
  } catch {
    return ''
  }
}

export function detectLanguage(): Language {
  const htmlLang = typeof document !== 'undefined'
    ? (document.documentElement.getAttribute('lang') ?? '').toLowerCase()
    : ''
  if (htmlLang.startsWith('zh')) return 'zh'
  if (htmlLang.startsWith('en')) return 'en'
  return storedLanguage().toLowerCase().startsWith('zh') ? 'zh' : 'en'
}

export function translate(zh: string, en: string, vars?: Vars): string {
  const template = detectLanguage() === 'zh' ? zh : en
  if (!vars) return template
  return template.replace(/\{(\w+)\}/g, (match, key: string) => {
    const value = vars[key]
    return value === undefined ? match : String(value)
  })
}
