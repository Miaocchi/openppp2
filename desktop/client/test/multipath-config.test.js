import test from 'node:test'
import assert from 'node:assert/strict'
import { defaultMultipathConfig, readMultipathConfig, validateMultipathConfig, writeMultipathConfig } from '../src/lib/multipathConfig.js'

const validPath = { id: 'edge-a', transport: 'wss', connect_host: 'cdn.example.invalid', port: 443, connections: 1, tls_server_name: 'cdn.example.invalid', http_host: 'cdn.example.invalid', ws_path: '/tunnel' }

test('multipath config defaults disabled and edits preserve the existing appsettings document', () => {
  const initial = { concurrent: 2, client: { guid: 'client', server: 'ppp://example.invalid:443/' }, key: { psk: 'kept-outside-the-form' } }
  const defaults = defaultMultipathConfig()
  assert.equal(defaults.enabled, false)
  const edited = writeMultipathConfig(initial, { ...defaults, paths: [validPath] })
  assert.equal(edited.client.server, initial.client.server)
  assert.deepEqual(edited.key, initial.key)
  assert.deepEqual(readMultipathConfig(edited).paths, [validPath])
})

test('multipath configuration validates supported transports, bounds and required WSS identity', () => {
  const root = { client: { multipath: { ...defaultMultipathConfig(), enabled: true, paths: [validPath] } } }
  assert.equal(validateMultipathConfig(root), true)
  assert.throws(() => validateMultipathConfig({ client: { multipath: { ...root.client.multipath, paths: [] } } }), /1 to 8 paths/)
  assert.throws(() => validateMultipathConfig({ client: { multipath: { ...root.client.multipath, paths: [{ ...validPath, tls_server_name: '' }] } } }), /requires a TLS server name/)
  assert.throws(() => validateMultipathConfig({ client: { multipath: { ...root.client.multipath, paths: [{ ...validPath, ws_path: '/x%0d%0aInjected' }] } } }), /WebSocket path is invalid/)
  assert.throws(() => validateMultipathConfig({ client: { multipath: { ...root.client.multipath, max_connections: 0 } } }), /between 1 and 32/)
})

test('multipath URL shorthand preserves domain identity and explicit dial address syntax', () => {
  const root = { client: { multipath: { ...defaultMultipathConfig(), enabled: true, paths: [{ id: 'lax', url: 'wss://vpn.example.com[203.0.113.10]:443/tun', connections: 1 }] } } }
  assert.equal(validateMultipathConfig(root), true)
  assert.throws(() => validateMultipathConfig({ client: { multipath: { ...root.client.multipath, paths: [{ ...root.client.multipath.paths[0], connect_host: '203.0.113.10' }] } } }), /cannot be combined/)
})
