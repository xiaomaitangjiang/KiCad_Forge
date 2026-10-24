import { useCallback, useEffect, useRef } from 'react'

// First-party plugin dialog chrome. The public SDK intentionally has no dialog
// slot; this keeps the plugin's import flow self-contained while reusing the
// host's modal CSS classes.
export function Modal({ title, children, onClose }) {
  const ref = useRef(null)
  const latest = useRef(onClose)
  latest.current = onClose
  const requestClose = useCallback(() => latest.current?.(), [])

  useEffect(() => {
    const previous = document.activeElement
    const first = ref.current?.querySelector('input, select, textarea, button')
    ;(first || ref.current)?.focus?.()
    const onKey = (event) => {
      if (event.key === 'Escape') {
        event.stopPropagation()
        requestClose()
      }
    }
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('keydown', onKey)
      previous?.focus?.()
    }
  }, [requestClose])

  return (
    <div className="modal-overlay show" onClick={(event) => { if (event.target === event.currentTarget) requestClose() }}>
      <div className="modal" ref={ref} role="dialog" aria-modal="true" aria-label={title} tabIndex={-1}>
        <div className="modal-header">
          <h3>{title}</h3>
          <button className="modal-close" aria-label="关闭" onClick={requestClose}>✕</button>
        </div>
        {children}
      </div>
    </div>
  )
}
