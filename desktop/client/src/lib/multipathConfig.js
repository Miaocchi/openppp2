export const defaultMultipathConfig = () => ({
  enabled: false,
  policy: 'adaptive',
  max_connections: 8,
  max_concurrent_connects: 4,
  resume_grace_ms: 30000,
  paths: [],
})

export function readMultipathConfig(root) {
  return root?.client?.multipath && typeof root.client.multipath === 'object' && !Array.isArray(root.client.multipath)
    ? root.client.multipath
    : defaultMultipathConfig()
}

export function writeMultipathConfig(root, multipath) {
  return {
    ...root,
    client: {
      ...(root.client || {}),
      multipath,
    },
  }
}

function integerIn(value, min, max) {
  return Number.isInteger(value) && value >= min && value <= max
}

function validHost(value) {
  return typeof value === 'string' && value.length > 0 && value.length <= 255 && /^[A-Za-z0-9.:[\]-]+$/.test(value)
}

function validWsPath(value) {
  if (typeof value !== 'string' || !value.startsWith('/') || value.length > 2048 || /[^\x21-\x7e\\?#]/.test(value)) return false
  return !/%(?:0a|0d|23)/i.test(value)
}

export function validateMultipathConfig(root) {
  const configured = root?.client?.multipath
  if (configured == null) return true
  if (typeof configured !== 'object' || Array.isArray(configured)) throw new Error('client.multipath must be an object')
  const multipath = { ...defaultMultipathConfig(), ...configured }
  if (typeof multipath.enabled !== 'boolean') throw new Error('client.multipath.enabled must be a boolean')
  if (!['adaptive', 'failover'].includes(multipath.policy)) throw new Error('Choose adaptive or failover scheduling')
  if (!integerIn(multipath.max_connections, 1, 32)) throw new Error('Maximum connections must be between 1 and 32')
  if (!integerIn(multipath.max_concurrent_connects, 1, 8)) throw new Error('Concurrent connects must be between 1 and 8')
  if (!integerIn(multipath.resume_grace_ms, 1000, 30000)) throw new Error('Recovery grace must be between 1000 and 30000 ms')
  if (!Array.isArray(multipath.paths) || multipath.paths.length > 8 || (multipath.enabled && multipath.paths.length === 0)) {
    throw new Error('Enabled multipath requires 1 to 8 paths')
  }
  const ids = new Set()
  let total = 0
  for (const [index, path] of multipath.paths.entries()) {
    const prefix = `Path ${index + 1}`
    if (!path || typeof path !== 'object' || !/^[A-Za-z0-9._-]{1,64}$/.test(path.id || '') || ids.has(path.id)) {
      throw new Error(`${prefix}: enter a unique path ID`)
    }
    ids.add(path.id)
    if (!['tcp', 'ws', 'wss'].includes(path.transport)) throw new Error(`${prefix}: choose TCP, WS or WSS`)
    if (!validHost(path.connect_host)) throw new Error(`${prefix}: enter a valid connect host`)
    if (!integerIn(path.port, 1, 65535)) throw new Error(`${prefix}: port must be between 1 and 65535`)
    if (!integerIn(path.connections, 1, 32)) throw new Error(`${prefix}: connections must be between 1 and 32`)
    total += path.connections
    const tls = path.tls_server_name || ''
    const http = path.http_host || ''
    const wsPath = path.ws_path || ''
    if ((tls && !validHost(tls)) || (http && !validHost(http))) throw new Error(`${prefix}: TLS name or HTTP host is invalid`)
    if (wsPath && !validWsPath(wsPath)) throw new Error(`${prefix}: WebSocket path is invalid`)
    if (path.transport === 'tcp' && (tls || http || wsPath)) throw new Error(`${prefix}: TCP does not use WebSocket fields`)
    if (path.transport === 'ws' && tls) throw new Error(`${prefix}: TLS server name is only valid for WSS`)
    if (path.transport === 'wss' && !tls) throw new Error(`${prefix}: WSS requires a TLS server name`)
  }
  if (total > multipath.max_connections) throw new Error('Path connection counts exceed the session maximum')
  return true
}
