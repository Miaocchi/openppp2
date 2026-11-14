import test from 'node:test'
import assert from 'node:assert/strict'
import { normalizeProxyForwards } from '../src/lib/proxyForwards.js'

test('normalizes valid proxy forwards like the Rust backend', () => {
  assert.deepEqual(normalizeProxyForwards(' 22, 8080:3000 '), { value: '22,8080:3000', error: false })
  assert.deepEqual(normalizeProxyForwards(''), { value: '', error: false })
  assert.deepEqual(normalizeProxyForwards('22,,80'), { value: '22,80', error: false })
})

test('rejects invalid proxy forwards', () => {
  for (const bad of ['0', '65536', 'abc', '22:', ':22', '1:2:3', '-1', '1.5']) assert.equal(normalizeProxyForwards(bad).error, true, bad)
})
