import i18n from 'i18next'
import { initReactI18next } from 'react-i18next'
import en from './locales/en.json'
import zh from './locales/zh.json'

const saved = localStorage.getItem('kf-lang')
const browserLang = (navigator.language || '').split('-')[0]

// Plugin frontends are separate bundles; they follow the document language
// rather than importing the host's i18next instance. Keep <html lang> and a
// small DOM event in sync so an already loaded panel can re-render.
function applyDocumentLanguage(language) {
  if (typeof document === 'undefined') return
  const next = String(language || '').toLowerCase().startsWith('zh') ? 'zh-CN' : 'en'
  if (document.documentElement.lang !== next) document.documentElement.lang = next
  if (typeof window !== 'undefined') {
    window.dispatchEvent(new CustomEvent('kf:languagechange', { detail: { language: next } }))
  }
}

i18n.use(initReactI18next).init({
  resources: { en: { translation: en }, zh: { translation: zh } },
  lng: saved || (['zh', 'en'].includes(browserLang) ? browserLang : 'en'),
  fallbackLng: 'en',
  interpolation: { escapeValue: false },
})

i18n.on('languageChanged', (lng) => {
  localStorage.setItem('kf-lang', lng)
  applyDocumentLanguage(lng)
})

applyDocumentLanguage(i18n.language)

export default i18n
