// The power button and the keyboard shortcut must never start a second kernel
// or request elevation. Only the explicit administrator restart action does that.
export function connectionControls(state, kind) {
  const { connection, subscription: { nodes } } = state
  const busy = !!connection.pid || ['starting', 'connecting', 'connected', 'reconnecting', 'stopping'].includes(connection.status)
  const idle = !busy && connection.status !== 'recovery_pending'
  const selected = busy ? connection.currentNodeId : (nodes.find((node) => node.id === connection.currentNodeId)?.id || nodes[0]?.id || '')
  const needsElevation = kind !== 'demo' && state.settings.connectionMode !== 'proxy' && !state.administrator
  let action = null
  if (connection.status === 'recovery_pending') action = 'retryRecovery'
  else if (connection.status !== 'stopping') {
    if (busy) action = 'disconnect'
    else if (selected && !needsElevation) action = 'connect'
  }
  return { busy, idle, selected, needsElevation, action }
}

export function runConnectionAction(runtime, { action, selected }) {
  if (action === 'connect') return runtime.connect(selected)
  if (action === 'disconnect') return runtime.disconnect()
  if (action === 'retryRecovery') return runtime.retryRecovery()
}
