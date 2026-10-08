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
    ...createMockPolicy(),
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

// Demo policy commands: shaped like `ppp policy --json` reports, no kernel involved.
function createMockPolicy() {
  const report = (command, extra = {}) => ({ exitCode: 0, report: { schema: 1, command, status: 'ok', diagnostics: [], ...extra } })
  const template = () => ({
    policy: { version: 2, rules: { path: 'routing.rules' }, ipv6: 'block', dns: { mode: 'fake-ip', 'fake-ip': { range: '198.18.0.0/16', storage: './dns-fake-ip', identity: 'client-default' }, resolvers: { local: { via: 'direct', servers: ['doh.pub'] }, remote: { via: 'proxy', servers: ['cloudflare'] } } } },
    rules: 'default proxy\ndns direct local\ndns proxy remote\n',
  })
  let saved = null, enabled = false
  return {
    async policyLoad() { return { enabled, policyDir: 'C:\\Users\\demo\\AppData\\Roaming\\OpenPPP2\\policy', policy: saved?.policy || null, rules: saved?.rules || '', kernelSupported: true, kernelError: '' } },
    async policySetEnabled(value) { if (value && !saved) throw new Error('Create and save a policy before enabling it'); enabled = value; return value },
    async policySave(policy, rules) { saved = { policy, rules }; return { policy, rules, result: report('check') } },
    async policyCheck() { return report('check', { policy_version: 1, rule_count: 1 }) },
    async policyExplain(policy, rules, target) {
      return report('explain', { route: { action: 'proxy', matched: false, rule_id: 'default', source: 'default', file: 'routing.rules', line: 1, reason: 'default action' }, dns: { applicable: !/^[\d.]+$/.test(target), resolver: 'remote', via: 'proxy', rejected: false, rule_id: 'dns proxy', file: 'routing.rules', line: 3 } })
    },
    async policyInit() { return { ...template(), result: report('init', { template: 'proxy-all' }) } },
    async policyMigrate() { return { ...template(), result: { exitCode: 5, report: { schema: 1, command: 'migrate', status: 'draft', diagnostics: [], migration: { status: 'draft', routing_equivalence: 'unverified', whole_policy_equivalence: 'draft', diagnostics: [{ code: 'E_MIGRATE_DNS_RUNTIME', message: 'Legacy Fake-IP, cache, or ECS behavior differs from policy-scoped v2 DNS.' }] } } } } },
    async policyStatus() { throw new Error('No policy status yet: connect once with policy v2 enabled') },
    async policyUpdate() { throw new Error('Connect first: manual update uses the session\'s SOCKS listener') },
    async policyResetFakeIp() { return 'Fake-IP storage cleared' },
  }
}
