import { writable } from 'svelte/store'

// A running kernel keeps the configuration it started with. Retain only the last
// affected session for each page, not a boolean that also matches the next one.
export const pendingReconnect = writable({ routing: null, dns: null })

export function sessionKey(connection) {
  if (!['connected', 'reconnecting', 'connecting', 'starting'].includes(connection.status)) return null
  if (connection.sessionId > 0) return `kernel:${connection.sessionId}`
  return connection.connectedAt ? `demo:${connection.connectedAt}` : null
}

export function markPending(area, session) {
  if (session !== null) pendingReconnect.update((marks) => ({ ...marks, [area]: session }))
}
