import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './i18n'
import './App.css'
import App from './App.jsx'
import ErrorBoundary from './components/ErrorBoundary.jsx'

// 这里刻意不发 /api/bye：刷新页面（F5）和真正关闭本地窗口在浏览器里无法区分，
// 误发信标会让后台服务直接退出。真正关闭由本地窗口的生命周期处理，
// 心跳超时是最后的兜底。会话心跳见 App 里的 startDesktopSession。

createRoot(document.getElementById('root')).render(
  <StrictMode>
    <ErrorBoundary>
      <App />
    </ErrorBoundary>
  </StrictMode>,
)
