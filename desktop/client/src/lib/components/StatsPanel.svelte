<script>
  import { Copy } from 'lucide-svelte'
  import { formatBytes } from '../format.js'
  import { t } from '../i18n.js'
  export let stats
  export let stale=false
  let error=''
  async function copy(value) { try { await navigator.clipboard.writeText(value) } catch(cause) { error=String(cause) } }
</script>
<section class="stats-band"><div class="metrics">
  <div><span>{$t('Download','下载')}</span><strong>{stale ? '—' : stats.rxRateMbps.toFixed(1)} <small>Mbps</small></strong><small>{formatBytes(stats.rxBytes)}</small></div>
  <div><span>{$t('Upload','上传')}</span><strong>{stale ? '—' : stats.txRateMbps.toFixed(1)} <small>Mbps</small></strong><small>{formatBytes(stats.txBytes)}</small></div>
  <div><span>{$t('Active links','活动链路')}</span><strong>{stale ? '—' : stats.activeLinks}</strong><small>{stats.effectiveMuxMode || '—'} · {stats.effectivePath || '—'}</small></div>
  <div><span>{$t('Link quality','链路质量')}</span><strong>{stale || !stats.qualityGrade || stats.qualityGrade === 'Unknown' ? '—' : `${stats.qualityPercent.toFixed(1)}%`}</strong><small>{stats.qualityGrade || $t('Unknown','未知')}</small></div>
</div><div class="addresses">{#each [['HTTP',stats.httpProxy],['SOCKS',stats.socksProxy],['TUN',stats.tunIp]] as [label,value]}{#if value}<span><b>{label}</b><code>{value}</code><button class="icon-button" title={$t('Copy','复制')} aria-label={$t('Copy','复制')} on:click={() => copy(value)}><Copy size={13}/></button></span>{/if}{/each}</div>{#if error}<p class="error-line">{error}</p>{/if}</section>
<style>.stats-band { padding:24px 0; border-bottom:1px solid var(--border); } .metrics { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:18px; } .metrics div { display:grid; gap:10px; } .metrics span, small { font-size:12px; color:var(--text-3); } strong { font-size:23px; font-variant-numeric:tabular-nums; } .addresses { display:flex; flex-wrap:wrap; gap:16px; margin-top:20px; } .addresses span { display:flex; gap:8px; align-items:center; } b { font-size:11px; color:var(--text-3); } code { font-size:12px; } @media(max-width:760px) { .metrics { grid-template-columns:repeat(2,minmax(0,1fr)); } }</style>
