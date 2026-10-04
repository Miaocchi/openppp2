<script>
  import { onDestroy } from 'svelte'
  import { Power, X } from 'lucide-svelte'
  import { connectionStates } from '../runtime/model.js'
  import { formatDuration } from '../format.js'
  import { t } from '../i18n.js'
  export let state
  export let runtime
  let now=Date.now(), error=''
  const tick=setInterval(() => now=Date.now(),1000)
  onDestroy(() => clearInterval(tick))
  $: connection=state.connection
  $: status=connectionStates[connection.status] || connectionStates.disconnected
  $: busy=!!connection.pid || ['starting','connecting','connected','reconnecting','stopping'].includes(connection.status)
  $: selected=connection.currentNodeId || state.subscription.nodes[0]?.id || ''
  const labels={connected:'Connected',connecting:'Connecting',starting:'Starting',reconnecting:'Reconnecting',stopping:'Stopping',disconnected:'Disconnected',error:'Connection failed'}
  const phases={preparing_host:['Preparing adapter','准备网卡'],handshaking:['Authenticating','认证中'],applying_policy:['Applying policy','应用网络策略'],connecting:['Connecting to server','连接服务器']}
  async function act() { try { error=''; if(busy) await runtime.disconnect(); else await runtime.connect(selected) } catch(cause) { error=String(cause) } }
  async function mode(value) { try { await runtime.updateSetting('connectionMode',value) } catch(cause) { error=String(cause) } }
</script>
<section class="connection-area">
  <div class="connection-head"><div><div class="eyebrow">OpenPPP2 Client</div><h1 class={status.tone}><i></i>{$t(labels[connection.status] || 'Disconnected',status.label)}</h1></div>
    <button class="primary-button action" disabled={connection.status === 'stopping' || (!busy && !selected)} on:click={act}>{#if busy}<X size={17}/>{:else}<Power size={17}/>{/if}{$t(busy ? 'Disconnect' : connection.status === 'error' ? 'Retry' : 'Connect',status.action)}</button>
  </div>
  <div class="connection-controls"><label class="field"><span>{$t('Node','节点')}</span><select class="select-input" value={selected} disabled={busy} on:change={(event) => runtime.selectNode(event.currentTarget.value)}>{#if !state.subscription.nodes.length}<option value="">{$t('No nodes','暂无节点')}</option>{/if}{#each state.subscription.nodes as node}<option value={node.id}>{node.name}</option>{/each}</select></label>
    <div class="field"><span>{$t('Connection mode','连接方式')}</span><div class="segmented">{#each [['client','Virtual adapter','虚拟网卡'],['proxy','Local proxy','本地代理']] as [value,en,zh]}<button disabled={busy} class:active={state.settings.connectionMode === value} aria-pressed={state.settings.connectionMode === value} on:click={() => mode(value)}>{$t(en,zh)}</button>{/each}</div></div>
  </div>
  <div class="connection-meta">{#if connection.status === 'connected'}<span>{$t('Online','在线')} {formatDuration(connection.connectedAt,now)}</span>{/if}{#if phases[connection.phase]}<span>{$t(...phases[connection.phase])}</span>{/if}{#if busy && !connection.statsAvailable}<span>{$t('Waiting for kernel statistics','等待内核统计')}</span>{/if}{#if connection.statsStale}<span class="warning">{$t('Statistics stale','统计已过期')}</span>{/if}</div>
  {#if error || connection.lastError}<div role="alert" class="error-line">{error || connection.lastError}</div>{/if}
</section>
<style>
  .connection-area { padding:12px 0 24px; border-bottom:1px solid var(--border); } .connection-head { display:flex; justify-content:space-between; align-items:center; gap:16px; } .eyebrow { font-size:12px; color:var(--text-3); margin-bottom:8px; }
  h1 { margin:0; font-size:24px; display:flex; gap:12px; align-items:center; overflow-wrap:anywhere; } h1 i { width:10px; height:10px; border-radius:50%; background:var(--gray); flex:none; } h1.success i { background:var(--green); } h1.warning i { background:var(--yellow); } h1.danger i { background:var(--red); }
  .action { display:flex; gap:8px; align-items:center; flex:none; } .connection-controls { display:grid; grid-template-columns:minmax(0,1fr) minmax(220px,.7fr); gap:24px; margin-top:26px; } .connection-meta { display:flex; flex-wrap:wrap; gap:16px; font-size:12px; color:var(--text-2); margin-top:16px; }
  @media(max-width:760px) { .connection-controls { grid-template-columns:1fr; gap:16px; } }
</style>
