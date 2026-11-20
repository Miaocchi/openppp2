<script>
  import { onDestroy, onMount } from 'svelte'
  import { parseRules, listRules } from '../policyRules.js'
  import { Power, ChevronRight, ShieldAlert } from 'lucide-svelte'
  import { connectionStates } from '../runtime/model.js'
  import { sessionKey, pendingReconnect, markPending } from '../pendingReconnect.js'
  import { connectionControls, runConnectionAction } from '../connectionControls.js'
  import { formatDuration } from '../format.js'
  import { t } from '../i18n.js'
  export let state
  export let runtime
  let now=Date.now(), error='', routing=null, routingBusy=false
  let initialized=runtime.kind === 'demo', pickingKernel=false
  let applying=false
  const tick=setInterval(() => now=Date.now(),1000)
  onDestroy(() => clearInterval(tick))
  $: connection=state.connection
  $: session=sessionKey(connection)
  $: pendingRouting=session !== null && $pendingReconnect.routing === session
  $: pendingDns=session !== null && $pendingReconnect.dns === session
  $: status=connectionStates[connection.status] || connectionStates.disconnected
  $: controls=connectionControls(state,runtime.kind)
  $: ({ busy, idle, selected, needsElevation }=controls)
  $: nodes=state.subscription.nodes
  const labels={connected:'Connected',connecting:'Connecting',starting:'Starting',reconnecting:'Reconnecting',stopping:'Stopping',disconnected:'Disconnected',recovery_pending:'Network recovery pending',error:'Connection failed'}
  const actions={connected:'Disconnect',connecting:'Cancel',starting:'Cancel',reconnecting:'Cancel',stopping:'Stopping',disconnected:'Connect',recovery_pending:'Restore network',error:'Retry'}
  $: actionLabel=controls.action === 'disconnect' && !['starting','connecting','reconnecting'].includes(connection.status)
    ? $t('Disconnect','断开') : $t(actions[connection.status] || 'Connect',status.action)
  const phases={preparing_host:['Preparing adapter','准备网卡'],handshaking:['Authenticating','认证中'],applying_policy:['Applying policy','应用网络策略'],connecting:['Connecting to server','连接服务器']}
  const modes=[
    ['proxy','Proxy mode','代理模式','For apps using the local HTTP or SOCKS proxy, including apps that follow the system proxy when enabled. No administrator rights needed.','仅影响使用本地 HTTP 或 SOCKS 代理的应用。启用系统代理后，遵循系统代理设置的应用也可使用。无需管理员权限。'],
    ['client','Virtual adapter','虚拟网卡','Captures traffic that matches the adapter’s routes, even from apps without proxy support. Requires administrator rights.','接管匹配虚拟网卡路由的流量，包括不支持代理的应用流量。需要管理员权限。'],
  ]
  $: actionDisabled=!initialized || !controls.action || pickingKernel || applying
  async function act() {
    try {
      error=''
      if(!actionDisabled) await runConnectionAction(runtime,controls)
    } catch(cause) { error=String(cause) }
  }
  async function chooseKernel() {
    if(!idle || pickingKernel) return
    pickingKernel=true; error=''
    try { await runtime.pickExecutable() } catch(cause) { error=String(cause) } finally { pickingKernel=false }
  }
  async function mode(value) { try { error=''; await runtime.updateSetting('connectionMode',value) } catch(cause) { error=String(cause) } }
  onMount(async () => {
    try { await runtime.ready; initialized=true } catch { return } // App displays bootstrap errors.
    try { const info=await runtime.policyLoad(); routing={ enabled:!!info.enabled, exists:!!info.policy, rules:info.rules || '' } } catch { routing=null }
  })
  async function toggleRouting(event) {
    const checkbox=event.currentTarget
    const wanted=checkbox.checked
    const changedSession=session
    routingBusy=true; error=''
    try {
      const enabled=await runtime.policySetEnabled(wanted)
      if(enabled !== routing.enabled) markPending('routing',changedSession)
      routing={ ...routing, enabled }
    } catch(cause) { error=String(cause) } finally { routingBusy=false }
    checkbox.checked=!!routing?.enabled
  }
  async function applySaved() {
    if(applying || connection.status !== 'connected' || !connection.currentNodeId || !(pendingRouting || pendingDns)) return
    applying=true; error=''
    try { await runtime.switchNode(connection.currentNodeId) } catch(cause) { error=String(cause) } finally { applying=false }
  }
  const actionWords={ direct:['direct','直连'], proxy:['through the tunnel','走代理'], reject:['refused','拒绝'] }
  // One line that says what the policy does, e.g. "Direct all" or "6 rules · rest through the tunnel".
  function routingSummary(value) {
    if(!value?.exists) return $t('No policy — using the bypass list','未创建分流策略，使用直连列表')
    if(!value.enabled) return $t('Policy off — legacy bypass list in use','策略未启用 · 使用传统直连列表')
    const model=parseRules(value.rules), active=listRules(model).filter((rule) => rule.enabled)
    const rest=$t(...(actionWords[model.defaultAction] || [model.defaultAction,model.defaultAction]))
    if(!active.length) return $t(`Everything ${rest}`,`全部${rest}`)
    return $t(`${active.length} rules · everything else ${rest}`,`${active.length} 条规则 · 其余${rest}`)
  }
  async function elevate() {
    if(!idle || !needsElevation || !confirm($t('Restart the client as administrator to set up the virtual adapter? Windows may ask for approval.','以管理员身份重启客户端以配置虚拟网卡？Windows 可能会请求授权。'))) return
    try { error=''; await runtime.elevate() } catch(cause) { error=String(cause) }
  }
</script>

<section class="hero" aria-label={$t('Connection control','连接控制')}>
  {#if initialized && idle && !nodes.length}
    <div class="card first-node">
      <h2>{$t('Add your first node','添加第一个节点')}</h2>
      <p>{$t('If you have a subscription URL, add or refresh it in Subscriptions. For a single server, add its details in Nodes.','如果有订阅链接，请前往「订阅」添加或刷新来源；如果只有一台服务器，请在「节点」中填写其配置。')}</p>
      <div class="toolbar">
        <button class="primary-button" on:click={() => runtime.navigate('subscription')}>{$t('Open subscriptions','前往订阅')}</button>
        <button class="secondary-button" on:click={() => runtime.navigate('nodes')}>{$t('Add a node','添加节点')}</button>
      </div>
    </div>
  {:else}
  <div class="card power-card">
    <button id="connection-power" class={`power ${status.tone}`} disabled={actionDisabled} on:click={act} aria-label={actionLabel}>
      <Power size={38} strokeWidth={2}/>
      <span>{actionLabel}</span>
    </button>
    <h2 class={`state ${status.tone}`}><i></i>{$t(labels[connection.status] || 'Disconnected',status.label)}</h2>
    <div class="meta">
      {#if connection.status === 'connected'}<span>{$t('Online','在线')} {formatDuration(connection.connectedAt,now)}</span>{/if}
      {#if phases[connection.phase]}<span>{$t(...phases[connection.phase])}</span>{/if}
      {#if busy && !connection.statsAvailable}<span>{$t('Waiting for kernel statistics','等待内核统计')}</span>{/if}
      {#if connection.statsStale}<span class="warning">{$t('Statistics stale','统计已过期')}</span>{/if}
      {#if connection.status === 'recovery_pending'}<span class="warning">{$t('Network changes require recovery before reconnecting','网络状态仍待恢复，完成前不能重新连接')}</span>
      {:else if initialized && idle && needsElevation}<span>{$t('Choose proxy mode or restart as administrator.','请选择代理模式，或以管理员身份重启。')}</span>
      {:else if initialized && idle && connection.status !== 'error'}<span>{$t('Choose a node and access mode, then connect','选择节点和接入方式后连接')}</span>{/if}
    </div>
  </div>
  {/if}

  <div class="card setup-card">
    {#if nodes.length}
      <label class="field"><span class="label">{$t('Node','节点')}</span>
        <select class="select-input" value={selected} disabled={busy} on:change={(event) => runtime.selectNode(event.currentTarget.value)}>
          {#each nodes as node}<option value={node.id}>{node.name}</option>{/each}
        </select>
      </label>
    {/if}
    {#if initialized && idle && runtime.kind !== 'demo'}
      <div class="kernel-row">
        <div class="field"><span class="label">{$t('Connection kernel','连接内核')}</span>
          {#if state.settings.pppPath}<span class="kernel-path mono">{state.settings.pppPath}</span>
          {:else}<span>{$t('The client looks for ppp.exe beside its own executable. If your copy is elsewhere, select it here.','客户端默认在程序所在目录查找 ppp.exe。如果文件在其他位置，请在此选择。')}</span>{/if}
        </div>
        <button class="secondary-button" disabled={pickingKernel} on:click={chooseKernel}>{$t('Choose ppp.exe…','选择 ppp.exe…')}</button>
      </div>
    {/if}
    <div class="field">
      <span class="label" id="access-mode">{$t('Access mode','接入方式')}</span>
      <div class="choice-grid modes" role="radiogroup" aria-labelledby="access-mode">
        {#each modes as [value,en,zh,enHint,zhHint]}
          {@const active=(state.settings.connectionMode === 'proxy') === (value === 'proxy')}
          <button class="choice" role="radio" aria-checked={active} disabled={busy} on:click={() => mode(value)}><strong>{$t(en,zh)}</strong><small>{$t(enHint,zhHint)}</small></button>
        {/each}
      </div>
      {#if initialized && needsElevation}<div class="elevate"><ShieldAlert size={15}/><span>{$t('Windows requires administrator rights to configure the virtual adapter and its routes.','配置虚拟网卡及其路由需要 Windows 管理员权限。')}</span><button class="inline-link accent" disabled={!idle || pickingKernel} on:click={elevate}>{$t('Restart as administrator','以管理员身份重启')}</button></div>{/if}
    </div>
    <div class="routing-link">
      <button class="routing-open" on:click={() => runtime.navigate('routing')}>
        <span><small>{$t('Routing','分流')}</small><span>{routing ? routingSummary(routing) : (state.policyEnabled ? $t('Routing policy in use','使用分流策略') : $t('Legacy routing (bypass list)','传统分流（直连列表）'))}</span></span>
        <span class="edit">{$t('Edit','编辑')}<ChevronRight size={15}/></span>
      </button>
      {#if routing?.exists}<input type="checkbox" role="switch" checked={routing.enabled} disabled={routingBusy} aria-label={$t('Use the routing policy','启用分流策略')} title={busy ? $t('Takes effect after reconnecting','重连后生效') : ''} on:change={toggleRouting}/>{/if}
    </div>
    {#if connection.status === 'connected' && (pendingRouting || pendingDns)}
      <div class="notice warn" role="note">
        <span>{pendingRouting && pendingDns ? $t('Reconnect to apply saved routing and DNS settings.','重连后应用已保存的分流和 DNS 设置。') : pendingRouting ? $t('Reconnect to apply saved routing settings.','重连后应用已保存的分流设置。') : $t('Reconnect to apply saved DNS settings.','重连后应用已保存的 DNS 设置。')}</span>
        <button class="primary-button" disabled={applying || !connection.currentNodeId} on:click={applySaved}>{$t('Reconnect now','立即重连生效')}</button>
      </div>
    {:else if busy && routing?.exists}<small class="subtle hint">{$t('Routing changes take effect after reconnecting.','分流修改在重连后生效。')}</small>{/if}
  </div>
</section>
{#if error || connection.lastError}<div role="alert" class="error-line">{error || connection.lastError}</div>{/if}

<style>
  .hero { display:grid; grid-template-columns:repeat(auto-fit,minmax(min(280px,100%),1fr)); gap:16px; }
  .first-node { padding:24px; display:grid; align-content:center; justify-items:start; gap:12px; }
  .first-node h2 { font-size:18px; margin:0; }
  .first-node p { color:var(--text-2); line-height:1.55; margin:0; }
  .kernel-row { display:flex; align-items:center; flex-wrap:wrap; gap:10px; border-bottom:1px solid var(--border); padding-bottom:14px; font-size:13px; }
  .kernel-row .field { flex:1 1 200px; min-width:0; }
  .kernel-path { overflow-wrap:anywhere; }
  .power-card { padding:26px 20px; display:grid; justify-items:center; align-content:center; gap:14px; text-align:center; }
  .power { width:140px; height:140px; border-radius:50%; border:2px solid var(--text-3); background:var(--surface); color:var(--text-2); display:grid; place-items:center; align-content:center; gap:6px; cursor:pointer; font-weight:600; font-size:14px; }
  .power:hover:not(:disabled) { border-color:var(--accent); color:var(--accent); }
  .power.success { border-color:var(--accent); color:var(--accent); background:var(--accent-soft); }
  .power.warning { border-color:var(--yellow); color:var(--yellow); }
  .power.danger { border-color:var(--red); color:var(--red); }
  .state { margin:0; font-size:18px; display:flex; gap:10px; align-items:center; } .state i { width:9px; height:9px; border-radius:50%; background:var(--gray); }
  .state.success i { background:var(--green); } .state.warning i { background:var(--yellow); } .state.danger i { background:var(--red); }
  .meta { display:flex; flex-wrap:wrap; justify-content:center; gap:6px 14px; font-size:12px; color:var(--text-2); min-height:18px; }
  .setup-card { padding:20px; display:grid; gap:18px; align-content:start; }
  .label { font-size:12px; color:var(--text-2); }
  .modes { grid-template-columns:repeat(2,minmax(0,1fr)); }
  .choice strong { font-size:14px; }
  .elevate { display:flex; flex-wrap:wrap; align-items:center; gap:8px; font-size:12px; color:var(--yellow); margin-top:4px; }
  .accent { color:var(--accent); } .accent:hover { color:var(--text); }
  .routing-link { display:flex; align-items:center; gap:12px; min-height:52px; padding:0 14px 0 0; border-radius:10px; border:1px solid var(--border-strong); background:var(--surface); color:var(--text); }
  .routing-open { flex:1; min-width:0; display:flex; align-items:center; justify-content:space-between; gap:12px; padding:8px 0 8px 14px; border:0; background:none; color:inherit; font:inherit; cursor:pointer; text-align:left; }
  .routing-open > span:first-child { display:grid; gap:2px; min-width:0; } .routing-link small { color:var(--text-2); font-size:12px; }
  .hint { margin-top:-10px; font-size:12px; }
  .routing-link .edit { display:inline-flex; align-items:center; gap:2px; color:var(--accent); font-size:13px; flex:none; }
  .routing-link:hover, .routing-link:focus-within { border-color:var(--accent); }
  @media(max-width:520px) { .modes { grid-template-columns:1fr; } }
</style>
