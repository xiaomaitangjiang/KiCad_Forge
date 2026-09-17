import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './i18n'
import './App.css'
import App from './App.jsx'
import ErrorBoundary from './components/ErrorBoundary.jsx'

// Tell the backend we are going away on window close — the server then stops
// within ~200ms instead of waiting for the heartbeat timeout.
window.addEventListener('beforeunload', () => {
  navigator.sendBeacon('/api/bye')
})

createRoot(document.getElementById('root')).render(
  <StrictMode>
    <ErrorBoundary>
      <App />
    </ErrorBoundary>
  </StrictMode>,
)
