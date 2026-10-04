import { createClientState, createEmptyClientState } from './model.js'

function clone(value) {
  return structuredClone(value)
}

export function createMockRuntime() {
  let state = createClientState()
  const empty=createEmptyClientState()
  state={ ...empty,...state, connection:{...empty.connection,...state.connection}, settings:{...empty.settings,...state.settings}, subscription:{...empty.subscription,...state.subscription} }
  state.networkOverrides={}; state.history=[]
  const listeners = new Set()
  let transitionTimer = null

  function emit() {
    const snapshot = clone(state)
    listeners.forEach((listener) => listener(snapshot))
  }

  function appendEvent(message, severity = 'info') {
    state.events = [
      ...state.events,
      {
        id: Date.now() + Math.random(),
        time: new Date().toLocaleTimeString('zh-CN', { hour12: false }),
        message,
        severity,
      },
    ].slice(-200)
  }

  function finishConnection(nodeId) {
    const node = state.subscription.nodes.find((item) => item.id === nodeId)
    state.connection.status = 'connected'
    state.connection.currentNodeId = nodeId
    state.connection.connectedAt = Date.now()
    state.connection.exitCode = null
    appendEvent(`session established role=main node=${node?.name || nodeId}`, 'success')
    emit()
  }

  return {
    ready: Promise.resolve(),
    selectNode(nodeId) { state.connection.currentNodeId=nodeId; emit() },
    async updateNetwork(overrides) { state.networkOverrides=structuredClone(overrides); emit(); return overrides },
    async saveSubscription(source) { const saved={...source,id:source.id || `s${Date.now()}`}; state.subscription.sources=[...state.subscription.sources.filter((s) => s.id !== saved.id),saved]; emit() },
    async deleteSubscription(sourceId) { state.subscription.sources=state.subscription.sources.filter((s) => s.id !== sourceId); emit() },
    async restoreProxy() { state.proxyRecoveryPending=false; emit() },
    async inspectKernel() { throw new Error('Kernel inspection requires the Windows desktop app') },
    async pickExecutable() { throw new Error('File selection requires the Windows desktop app') },
    async elevate() { throw new Error('Elevation requires the Windows desktop app') },
    async openData() { throw new Error('Opening app data requires the Windows desktop app') },
    async preview() { return {config:state.networkOverrides,args:[]} },
    async probeNodes() { emit() },
    subscribe(listener) {
      listeners.add(listener)
      listener(clone(state))
      return () => listeners.delete(listener)
    },
    navigate(route) {
      state.route = route
      emit()
    },
    connect(nodeId = state.connection.currentNodeId || state.subscription.nodes[0].id) {
      clearTimeout(transitionTimer)
      state.connection.status = 'connecting'
      state.connection.currentNodeId = nodeId
      state.connection.statsAvailable = false
      appendEvent(`tcp connecting ${nodeId}`)
      emit()
      transitionTimer = setTimeout(() => {
        state.connection.statsAvailable = true
        appendEvent('exchanger connected', 'success')
        finishConnection(nodeId)
      }, 900)
    },
    disconnect() {
      clearTimeout(transitionTimer)
      state.connection.status = 'disconnected'
      state.connection.connectedAt = null
      state.connection.statsAvailable = false
      appendEvent('process exited code=0')
      emit()
    },
    cancel() {
      clearTimeout(transitionTimer)
      state.connection.status = 'disconnected'
      state.connection.statsAvailable = false
      appendEvent('connection cancelled')
      emit()
    },
    switchNode(nodeId) {
      this.connect(nodeId)
    },
    toggleFavorite(nodeId) {
      state.subscription.nodes = state.subscription.nodes.map((node) =>
        node.id === nodeId ? { ...node, favorite: !node.favorite } : node,
      )
      emit()
    },
    async refreshSubscription() {
      state.subscription.lastSyncedAt = new Date().toISOString()
      state.subscription.cached = false
      state.subscription.cacheAgeMinutes = 0
      appendEvent('subscription refreshed', 'success')
      emit()
    },
    updateConfig(config) {
      state.config = config
      emit()
    },
    async updateClientConfig(config, options) {
      state.config = config
      state.launchOptions = structuredClone(options)
      emit()
      return state.launchOptions
    },
    async saveManualNode(node) {
      const id = node.id || `manual:${Date.now()}`
      const server = node.config?.client?.server || ''
      const address = server.replace(/^ppp:\/\/(?:wss?\/)?/, '').split('/')[0]
      const saved = { ...node, id, address, latencyMs: null, favorite: false, source: 'manual' }
      const index = state.subscription.nodes.findIndex((item) => item.id === id)
      if (index >= 0) state.subscription.nodes[index] = saved
      else state.subscription.nodes = [saved, ...state.subscription.nodes]
      emit()
      return state.subscription
    },
    async deleteManualNode(nodeId) {
      state.subscription.nodes = state.subscription.nodes.filter((node) => node.id !== nodeId)
      emit()
      return state.subscription
    },
    async updateLaunchOptions(options) {
      state.launchOptions = structuredClone(options)
      emit()
      return state.launchOptions
    },
    updateSetting(key, value) {
      state.settings = { ...state.settings, [key]: value }
      emit()
    },
    clearEvents() {
      state.events = []
      emit()
    },
    simulate(status) {
      clearTimeout(transitionTimer)
      state.connection.status = status
      state.connection.statsAvailable = status === 'connected'
      state.connection.exitCode = status === 'error' ? -1 : null
      if (status === 'error') appendEvent('authentication failed / server rejected', 'error')
      emit()
    },
  }
}
