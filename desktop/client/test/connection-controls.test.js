import test from 'node:test'
import assert from 'node:assert/strict'
import { get } from 'svelte/store'
import { createEmptyClientState } from '../src/lib/runtime/model.js'
import { connectionControls, runConnectionAction } from '../src/lib/connectionControls.js'
import { sessionKey, markPending, pendingReconnect } from '../src/lib/pendingReconnect.js'

test('connection actions use node and privilege guards, but never require a custom kernel path', async () => {
  const state = createEmptyClientState()
  const controls = () => connectionControls(state, 'tauri')
  const calls = []
  const runtime = { connect: (id) => calls.push(id), disconnect: () => calls.push('stop'), retryRecovery: () => calls.push('recover') }
  state.settings.connectionMode = 'proxy'
  state.connection.currentNodeId = 'removed'
  await runConnectionAction(runtime, controls())
  state.subscription.nodes = [{ id: 'first' }]
  state.settings.connectionMode = 'client'
  await runConnectionAction(runtime, controls())
  assert.deepEqual(calls, [])
  state.settings.connectionMode = 'proxy'
  await runConnectionAction(runtime, controls())
  assert.deepEqual(calls, ['first'])
  state.subscription.nodes = []
  state.settings.connectionMode = 'client'
  for (const status of ['starting', 'connecting', 'connected', 'reconnecting']) {
    state.connection.status = status
    assert.equal(controls().action, 'disconnect')
  }
  state.connection.status = 'stopping'
  assert.equal(controls().action, null)
  state.connection.status = 'recovery_pending'
  await runConnectionAction(runtime, controls())
  assert.deepEqual(calls, ['first', 'recover'])
})

test('pending markers belong to a kernel session, not a node, page or connection timestamp', () => {
  const live = { status: 'connected', sessionId: 4, connectedAt: 100 }
  const session = sessionKey(live)
  markPending('dns', session)
  markPending('routing', session)
  assert.deepEqual(get(pendingReconnect), { dns: session, routing: session })
  assert.equal(sessionKey({ ...live, status: 'reconnecting', connectedAt: 250 }), session)
  assert.notEqual(sessionKey({ ...live, sessionId: 5 }), session)
  assert.equal(sessionKey({ ...live, status: 'disconnected' }), null)
  assert.notEqual(sessionKey({ ...live, sessionId: 0 }), sessionKey({ ...live, sessionId: 0, connectedAt: 200 }))
})
