// React binding for plugin-local translations. The host keeps <html lang> and
// a kf:languagechange event in sync for independently built plugin bundles.
import { useCallback, useEffect, useState } from 'react'
import { detectLanguage, translate } from './i18n-text.ts'
import type { Language, Vars } from './i18n-text.ts'

export { detectLanguage, translate }
export type { Language, Vars }

export function useLanguage(): Language {
  const [language, setLanguage] = useState<Language>(detectLanguage)
  useEffect(() => {
    const sync = () => setLanguage(detectLanguage())
    window.addEventListener('kf:languagechange', sync)
    const observer = typeof MutationObserver === 'function'
      ? new MutationObserver(sync)
      : null
    observer?.observe(document.documentElement, { attributes: true, attributeFilter: ['lang'] })
    return () => {
      window.removeEventListener('kf:languagechange', sync)
      observer?.disconnect()
    }
  }, [])
  return language
}

export function useT() {
  const language = useLanguage()
  return useCallback(
    (zh: string, en: string, vars?: Vars) => {
      const template = language === 'zh' ? zh : en
      if (!vars) return template
      return template.replace(/\{(\w+)\}/g, (match, key: string) => {
        const value = vars[key]
        return value === undefined ? match : String(value)
      })
    },
    [language],
  )
}
