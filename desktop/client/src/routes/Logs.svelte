<script>
  import { Download, Search, Trash2, Pause, Play, Copy } from 'lucide-svelte'
  import { t } from '../lib/i18n.js'
  import { downloadText, redactText } from '../lib/security.js'
  export let state
  export let runtime
  let query='', severity='all', session='all', paused=false, frozen=[], feedback='', container
  $: stream=paused ? frozen : state.events
  $: sessions=[...new Set(stream.map((event) => event.sessionId).filter((id) => id != null))]
  $: urls=[state.subscription.url,...(state.subscription.sources || []).map((s) => s.url)]
  $: events=stream.filter((event) => (severity === 'all' || event.severity === severity) && (session === 'all' || String(event.sessionId) === session))
    .map((event) => ({...event,message:redactText(event.message,urls)})).filter((event) => event.message.toLowerCase().includes(query.trim().toLowerCase()))
  $: filtering=!!query.trim() || severity !== 'all' || session !== 'all'
  $: if(!paused && container && events.length) requestAnimationFrame(() => { if(container) container.scrollTop=container.scrollHeight })
  function reset() { query=''; severity='all'; session='all'; feedback='' }
  function toggle() { if(!paused) frozen=state.events.slice(); paused=!paused; feedback='' }
  function exportText() { return events.map((e) => `${e.time}\t${e.severity}\t${e.message}`).join('\n') }
  async function copy() { try { await navigator.clipboard.writeText(exportText()); feedback=$t('Visible entries copied','已复制可见记录') } catch { feedback=$t('Could not copy to clipboard. Try Export instead.','无法复制到剪贴板，请使用导出。') } }
  async function clear() { await runtime.clearEvents(); frozen=[]; reset() }
</script>
<div class="page">
  <div class="page-heading"><div><h1>{$t('Logs','日志')}</h1><p class="page-lead">{$t('Filter session events before sharing. Copy and export include only visible entries with sensitive fields redacted.','可按会话筛选事件。复制和导出仅包含可见记录，并遮盖敏感字段。')}</p></div></div>
  <section class="card"><div class="card-head"><div><h2>{paused ? $t('Paused snapshot','已暂停显示') : $t('Live events','实时事件')}</h2><small class="subtle" role="status">{paused ? $t('New events continue to be collected.','新事件仍会记录。') : $t('Following new entries','自动跟随新记录')}</small></div>
    <div class="toolbar"><button class="secondary-button action" on:click={toggle}>{#if paused}<Play size={15}/>{$t('Resume','继续')}{:else}<Pause size={15}/>{$t('Pause','暂停')}{/if}</button>
      <button class="secondary-button action" disabled={!events.length} on:click={copy}><Copy size={15}/>{$t('Copy','复制')}</button>
      <button class="secondary-button action" disabled={!events.length} on:click={() => downloadText('openppp2-client.log',exportText())}><Download size={15}/>{$t('Export','导出')}</button>
      <button class="icon-button" disabled={!state.events.length && !frozen.length} title={$t('Clear','清空')} aria-label={$t('Clear','清空')} on:click={clear}><Trash2 size={15}/></button></div></div>
    <div class="toolbar filters"><label class="search"><Search size={15}/><input class="text-input" bind:value={query} placeholder={$t('Search logs','搜索日志')} aria-label={$t('Search logs','搜索日志')}/></label>
      <select class="select-input" bind:value={severity} aria-label={$t('Severity','级别')}><option value="all">{$t('All levels','全部级别')}</option><option value="info">{$t('Info','信息')}</option><option value="success">{$t('Success','成功')}</option><option value="warning">{$t('Warning','警告')}</option><option value="error">{$t('Error','错误')}</option></select>
      <select class="select-input" bind:value={session} aria-label={$t('Session','会话')}><option value="all">{$t('All sessions','全部会话')}</option>{#each sessions as id}<option value={String(id)}>#{id}</option>{/each}</select>
      <button class="inline-link" disabled={!filtering} on:click={reset}>{$t('Reset filters','重置筛选')}</button><span class="subtle">{events.length} / {stream.length}</span>
    </div>
    {#if feedback}<div class="feedback" role="status">{feedback}</div>{/if}
    <div class="log-list mono" bind:this={container}>{#each events as event (event.id)}
      <div class="log-row"><time>{event.time}</time><span>{event.severity}</span><code class:fault={event.severity === 'error'}>{event.message}</code></div>
    {:else}<div class="blank-state"><strong>{filtering ? $t('No matching logs','没有匹配的日志') : paused ? $t('Snapshot is empty','快照为空') : $t('No events yet','暂无事件')}</strong><p>{filtering ? $t('Reset filters to see all entries.','重置筛选以查看所有记录。') : paused ? $t('Resume to see new entries.','继续显示以查看新记录。') : $t('New session events will appear here.','新的会话事件将在此显示。')}</p></div>{/each}</div>
  </section>
</div>
<style>
  .filters { padding:14px 16px; } .filters .select-input { width:auto; } .search { display:flex; align-items:center; gap:8px; flex:1 1 180px; min-width:0; }
  .log-list { max-height:64vh; min-height:160px; overflow:auto; background:var(--surface); border-radius:0 0 12px 12px; border-top:1px solid var(--border); }
  .log-row { display:grid; grid-template-columns:74px 64px minmax(0,1fr); gap:8px; padding:9px 16px; font-size:12px; border-bottom:1px solid var(--border); }
  time,.log-row > span,.feedback { color:var(--text-2); } .feedback { padding:0 16px 10px; } code { overflow-wrap:anywhere; white-space:pre-wrap; } .fault { color:var(--red); }
  @media(max-width:600px) { .log-row { grid-template-columns:auto 1fr; } code { grid-column:1/-1; } .search { flex-basis:100%; } }
</style>
