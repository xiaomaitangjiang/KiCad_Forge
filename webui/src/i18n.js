import i18n from 'i18next'
import { initReactI18next } from 'react-i18next'
import en from './locales/en.json'
import zh from './locales/zh.json'

const saved = localStorage.getItem('kf-lang')
const browserLang = (navigator.language || '').split('-')[0]

i18n.use(initReactI18next).init({
  resources: { en: { translation: en }, zh: { translation: zh } },
  lng: saved || (['zh', 'en'].includes(browserLang) ? browserLang : 'en'),
  fallbackLng: 'en',
  interpolation: { escapeValue: false },
})

i18n.on('languageChanged', (lng) => localStorage.setItem('kf-lang', lng))

export default i18n
