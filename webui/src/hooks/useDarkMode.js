import { useState, useCallback } from 'react'

export function useDarkMode() {
  const [dark, setDark] = useState(() => localStorage.getItem('kf-dark') === '1')

  const toggleDark = useCallback(() => {
    setDark(prev => {
      const next = !prev
      localStorage.setItem('kf-dark', next ? '1' : '0')
      return next
    })
  }, [])

  return { dark, toggleDark }
}
