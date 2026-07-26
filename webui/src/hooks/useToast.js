import { useState, useCallback, useRef } from 'react'

export function useToast() {
  const [toast, setToast] = useState('')
  const timer = useRef(null)

  const toastMsg = useCallback((msg) => {
    setToast(msg)
    clearTimeout(timer.current)
    timer.current = setTimeout(() => setToast(''), 2800)
  }, [])

  return { toast, toastMsg }
}
