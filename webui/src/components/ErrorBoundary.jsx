import { Component } from 'react'

// Last line of defense: a crash in any child must not blank the whole app.
// Without this, React 19 unmounts the entire tree on an uncaught render error.
export default class ErrorBoundary extends Component {
  state = { error: null }

  static getDerivedStateFromError(error) {
    return { error }
  }

  componentDidCatch(error, info) {
    console.error('ErrorBoundary caught:', error, info)
  }

  render() {
    if (this.state.error) {
      return (
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', height: '100vh', gap: 12, padding: 24, textAlign: 'center' }}>
          <div style={{ fontSize: 40 }}>⚠️</div>
          <h3 style={{ margin: 0 }}>Something went wrong</h3>
          <p style={{ opacity: 0.7, margin: 0, maxWidth: 480, wordBreak: 'break-all' }}>
            {String(this.state.error?.message || this.state.error)}
          </p>
          <button
            className="btn-primary"
            style={{ padding: '8px 18px', borderRadius: 6, cursor: 'pointer' }}
            onClick={() => this.setState({ error: null })}>
            Try again
          </button>
        </div>
      )
    }
    return this.props.children
  }
}
