<script>
  import { t } from '../lib/i18n.js'
  export let state

  function parseConfig(text) {
    try { return JSON.parse(text) } catch { return {} }
  }
  function bytes(value) {
    return Number.isFinite(value) ? new Intl.NumberFormat(undefined, { maximumFractionDigits: 0 }).format(value) : '—'
  }
  function stateLabel(value) {
    const labels = {
      waiting: ['Waiting', '等待'], connecting: ['Connecting', '连接中'],
      authenticating: ['Authenticating', '认证中'], joined: ['Available', '可用'],
      backoff: ['Retry scheduled', '等待重试'], stopped: ['Stopped', '已停止'],
      active: ['Active', '活跃'], creating: ['Creating', '创建中'],
      recovering: ['Recovering', '恢复中'], failed: ['Failed', '失败'], idle: ['Idle', '空闲'],
    }
    const label = labels[value]
    return label ? $t(label[0], label[1]) : $t('Unknown', '未知')
  }

  $: config = parseConfig(state.config)
  $: multipathConfig = config?.client?.multipath || {}
  $: report = state.stats?.multipath || null
  $: configuredPaths = Array.isArray(multipathConfig.paths) ? multipathConfig.paths : []
  $: observedPaths = report?.paths || []
  $: rows = observedPaths.length ? observedPaths : configuredPaths.map((path) => ({ id: path.id, state: 'unknown' }))
</script>

<div class="page">
  <div class="section-heading"><div><h1>{$t('Paths', '多路径')}</h1><p class="subtle">{multipathConfig.policy || report?.policy || '—'} · {$t(multipathConfig.enabled ? 'Enabled in config' : 'Disabled in config', multipathConfig.enabled ? '配置已启用' : '配置已关闭')}</p></div></div>
  <section class="summary" aria-label={$t('Multipath session status', '多路径会话状态')}>
    <div><span class="label">{$t('Session state', '会话状态')}</span><strong>{stateLabel(report?.state)}</strong></div>
    <div><span class="label">{$t('Active paths', '可用路径')}</span><strong>{report ? report.activePaths : '—'}</strong></div>
    <div><span class="label">{$t('Recovery grace remaining', '剩余恢复宽限期')}</span><strong>{report?.remainingGraceMs != null ? `${report.remainingGraceMs} ms` : '—'}</strong></div>
  </section>

  {#if report?.capabilities?.length}
    <div class="capabilities"><span class="label">{$t('Negotiated capabilities', '已协商能力')}</span><span>{report.capabilities.join(' · ')}</span></div>
  {/if}

  {#if rows.length}
    <div class="table-scroll">
      <table>
        <thead><tr>
          <th>{$t('Path', '路径')}</th><th>{$t('State', '状态')}</th><th>{$t('Carriers', '承载数')}</th>
          <th>{$t('RTT', '往返时延')}</th><th>{$t('Received', '接收字节')}</th><th>{$t('Sent', '发送字节')}</th>
          <th>{$t('In flight', '在途字节')}</th><th>{$t('Retransmitted', '重传字节')}</th>
          <th>{$t('Reconnects', '重连次数')}</th><th>{$t('Error code', '错误代码')}</th>
        </tr></thead>
        <tbody>{#each rows as path (path.id)}
          <tr>
            <td class="mono">{path.id}</td><td>{stateLabel(path.state)}</td>
            <td>{path.carriers ?? '—'}</td><td>{path.rttMs != null ? `${path.rttMs} ms` : '—'}</td>
            <td>{bytes(path.rxBytes)}</td><td>{bytes(path.txBytes)}</td>
            <td>{bytes(path.inflightBytes)}</td><td>{bytes(path.retransmitBytes)}</td>
            <td>{path.reconnectAttempts ?? '—'}</td><td class="mono">{path.errorCode || '—'}</td>
          </tr>
        {/each}</tbody>
      </table>
    </div>
  {:else}
    <div class="empty">{$t('No paths are configured.', '尚未配置路径。')}</div>
  {/if}
</div>

<style>
  .page { display:grid; gap:18px; }
  .section-heading p { margin:6px 0 0; }
  .summary { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:18px; padding:16px 0; border-top:1px solid var(--border); border-bottom:1px solid var(--border); }
  .summary div { display:grid; gap:7px; min-width:0; }
  .summary strong { font-size:15px; font-weight:600; overflow-wrap:anywhere; }
  .label { color:var(--text-2); font-size:12px; }
  .capabilities { display:flex; gap:12px; flex-wrap:wrap; color:var(--text-2); font-size:12px; }
  .table-scroll { overflow:auto; border-top:1px solid var(--border); }
  table { width:100%; min-width:900px; border-collapse:collapse; text-align:left; font-size:12px; }
  th,td { padding:11px 10px; border-bottom:1px solid var(--border); white-space:nowrap; }
  th { color:var(--text-2); font-weight:500; }
  .mono { font-family:var(--font-mono,monospace); }
  .empty { padding:20px 0; color:var(--text-2); }
  @media(max-width:620px) { .summary { grid-template-columns:1fr 1fr; } }
</style>
