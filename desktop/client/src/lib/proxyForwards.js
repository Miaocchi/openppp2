// Mirrors normalize_proxy_forwards in src-tauri/src/launch_options.rs:
// comma-separated `port` or `port:localPort` items, each 1-65535.
const port = (text) => (/^\d+$/.test(text.trim()) && Number(text) >= 1 && Number(text) <= 65535 ? String(Number(text)) : null)

// Returns { value, error }; value is the normalized string, error is true when invalid.
export function normalizeProxyForwards(input) {
  const items = []
  for (const item of String(input ?? '').split(',').map((part) => part.trim()).filter(Boolean)) {
    const parts = item.split(':')
    if (parts.length > 2) return { value: '', error: true }
    const ports = parts.map(port)
    if (ports.some((value) => value === null)) return { value: '', error: true }
    items.push(ports.join(':'))
  }
  return { value: items.join(','), error: false }
}
