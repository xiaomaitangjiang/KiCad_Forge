import test from 'node:test'
import assert from 'node:assert/strict'
import { EventEmitter } from 'node:events'
import { stopEdge } from '../support/cdp.mjs'

function childFixture() {
  return Object.assign(new EventEmitter(), {
    pid: 4242, exitCode: null, signalCode: null,
    kill() { assert.fail('must not kill a process after a failed taskkill') },
  })
}

test('stopEdge waits for a late child exit after taskkill reports a missing PID', async () => {
  const child = childFixture()
  await stopEdge(child, {
    platform: 'win32', exitTimeoutMs: 300,
    spawnKill(pid) {
      assert.equal(pid, child.pid)
      const killer = new EventEmitter()
      setImmediate(() => {
        killer.emit('exit', 128)
        setTimeout(() => { child.exitCode = 0; child.emit('exit', 0) }, 20)
      })
      return killer
    },
  })
  assert.equal(child.exitCode, 0)
})

test('stopEdge accepts a confirmed signal exit during taskkill', async () => {
  const child = childFixture()
  await stopEdge(child, {
    platform: 'win32', exitTimeoutMs: 300,
    spawnKill() {
      const killer = new EventEmitter()
      setImmediate(() => {
        child.signalCode = 'SIGTERM'
        child.emit('exit', null, 'SIGTERM')
        killer.emit('exit', 128)
      })
      return killer
    },
  })
  assert.equal(child.exitCode, null)
  assert.equal(child.signalCode, 'SIGTERM')
})

test('stopEdge keeps taskkill failure visible when the child remains alive', async () => {
  const child = childFixture()
  await assert.rejects(stopEdge(child, {
    platform: 'win32', exitTimeoutMs: 30,
    spawnKill() {
      const killer = new EventEmitter()
      setImmediate(() => killer.emit('exit', 128))
      return killer
    },
  }), /taskkill exited 128/)
  assert.equal(child.exitCode, null)
})

test('stopEdge does not treat a child error as proof that it exited', async () => {
  const child = childFixture()
  child.on('error', () => {})
  await assert.rejects(stopEdge(child, {
    platform: 'win32', exitTimeoutMs: 30,
    spawnKill() {
      const killer = new EventEmitter()
      setImmediate(() => {
        killer.emit('exit', 128)
        setImmediate(() => child.emit('error', new Error('termination denied')))
      })
      return killer
    },
  }), /taskkill exited 128/)
  assert.equal(child.exitCode, null)
  assert.equal(child.signalCode, null)
})
