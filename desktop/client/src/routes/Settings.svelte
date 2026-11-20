<script>
  import { FolderOpen, RefreshCw, ShieldCheck, Undo2, Download, Save, RotateCcw, Eye } from 'lucide-svelte'
  import { t } from '../lib/i18n.js'
  import { downloadText, redactText } from '../lib/security.js'
  import { effective, setPath as setIn, parseBase, completeListeners } from '../lib/overrides.js'
  import { normalizeProxyForwards } from '../lib/proxyForwards.js'
  import Config from './Config.svelte'
  export let state
  export let runtime

  const tabs=[['general','General','常规'],['kernel','Kernel','内核'],['network','Network','网络'],['advanced','Advanced','高级']]
  const listeners=[['client/http-proxy/bind','HTTP bind','HTTP 监听地址','text','127.0.0.1'],['client/http-proxy/port','HTTP port','HTTP 端口','number',8080],['client/socks-proxy/bind','SOCKS bind','SOCKS 监听地址','text','127.0.0.1'],['client/socks-proxy/port','SOCKS port','SOCKS 端口','number',1080]]
  const p2pModes=[['relay','Relay through server','经服务器中转'],['direct-preferred','Prefer P2P direct','优先 P2P 直连']]
  const legacyRouting=[['client/routing/ip/bypass','Bypass IP/CIDR','直连 IP/CIDR','lines','One address or range per line; these never enter the tunnel.','每行一个地址或网段，这些流量不进隧道。'],['client/routing/dns/rules','DNS rules','DNS 规则','lines','One rule per line.','每行一条规则。'],['client/routing/ip/routes','Routes (JSON)','路由（JSON）','json','',''],['client/routing/ip/peer-routes','Peer prefix routes (JSON)','Peer 前缀路由（JSON）','json','Routes to other clients\' LAN prefixes.','到其他客户端局域网前缀的路由。']]

  let tab='general', error='', busy=false, message=''
  let configDirty=false
  const introductions={
    general:['Everyday preferences save as you change them.','日常偏好在修改后自动保存。'],
    kernel:['Choose which executable starts the next session and inspect its capabilities.','选择下次连接使用的可执行文件，并检查其支持的功能。'],
    network:['Network overrides and adapter options have separate Save controls. Both take effect on the next connection.','网络覆盖和网卡参数分别保存，均在下次连接时生效。'],
    advanced:['Launch options, raw configuration and diagnostics. Preview reads saved values, not drafts.','启动参数、原始配置和诊断。预览读取已保存的值，而非草稿。'],
  }
  function selectTab(id) {
    if(id === tab) return
    if(configDirty && !confirm($t('Discard unsaved launch options and configuration edits?','放弃未保存的启动参数和配置更改？'))) return
    configDirty=false; applyRaw(); tab=id
  }
  function tabKeys(e,index) {
    const destination={ArrowRight:(index+1)%tabs.length,ArrowLeft:(index+tabs.length-1)%tabs.length,Home:0,End:tabs.length-1}[e.key]
    if(destination == null) return
    e.preventDefault(); e.currentTarget.parentElement.children[destination].focus()
  }
  $: base=parseBase(state.config)
  let overrides=structuredClone(state.networkOverrides || {}), raw=JSON.stringify(overrides,null,2), preview=null

  async function run(action) { busy=true; error=''; try { await action() } catch(cause) { error=String(cause) } finally { busy=false } }
  async function diagnostics() { const result=await runtime.preview(); downloadText('openppp2-diagnostics.json',redactText(JSON.stringify({kernel:state.kernel,connection:state.connection,preview:result,events:state.events},null,2))) }

  // Reactive so template calls re-run when the draft changes.
  $: get=(path,fallback) => effective(overrides,base,path,fallback)
  function set(path,value) { overrides=completeListeners(setIn(overrides,path,value),base); raw=JSON.stringify(overrides,null,2); message=''; error='' }
  function edit(path,type,value) {
    try { set(path,type === 'lines' ? value.split('\n').map((line) => line.trim()).filter(Boolean) : type === 'json' ? JSON.parse(value) : type === 'number' ? Number(value) : value) }
    catch(cause) { error=String(cause) }
  }
  $: display=(path,type) => { const value=get(path,type === 'lines' ? [] : type === 'json' ? [] : ''); return type === 'json' ? JSON.stringify(value,null,2) : Array.isArray(value) ? value.join('\n') : String(value) }
  function parseOverrides() {
    const value=JSON.parse(raw)
    if(!value || Array.isArray(value) || typeof value !== 'object') throw new Error($t('Network overrides must be a JSON object.','网络覆盖必须是 JSON 对象。'))
    return value
  }
  function applyRaw() { try { overrides=parseOverrides(); error='' } catch(cause) { error=String(cause) } }
  async function saveOverrides() { await run(async () => { const value=parseOverrides(); await runtime.updateNetwork(value); overrides=value; raw=JSON.stringify(value,null,2); message=$t('Saved; applies on next connection','已保存，下次连接生效') }) }
  function resetOverrides() { overrides=structuredClone(state.networkOverrides || {}); raw=JSON.stringify(overrides,null,2); message=''; error='' }
  async function inspect() { await run(async () => { preview=await runtime.preview() }) }

  $: dirty=raw !== JSON.stringify(state.networkOverrides || {},null,2)
  $: policyOwnsRouting=!!state.policyEnabled || !!base?.client?.policy
</script>

<div class="page">
  <div class="page-heading"><div><h1>{$t('Settings','设置')}</h1><p class="page-lead">{$t(...introductions[tab])}</p></div></div>
  <div class="tabs" role="tablist" aria-label={$t('Settings sections','设置分区')}>{#each tabs as [id,en,zh],index}<button id={'tab-'+id} role="tab" aria-controls="settings-content" aria-selected={tab === id} class:active={tab === id} on:keydown={(e) => tabKeys(e,index)} on:click={() => selectTab(id)}>{$t(en,zh)}</button>{/each}</div>
  <div class="tab-content" role="tabpanel" id="settings-content" aria-labelledby={'tab-'+tab}>
    {#if tab === 'network' || tab === 'advanced'}<div class="draft-bar" class:changed={dirty}>
      <div class="draft-context"><small>{$t('Network overrides','网络覆盖')}</small><strong role="status">{dirty ? $t('Unsaved network changes','网络设置未保存') : message || $t('No unsaved changes','没有未保存的修改')}</strong></div>
      <div class="toolbar"><button class="icon-button" title={$t('Discard changes','放弃修改')} aria-label={$t('Discard changes','放弃修改')} disabled={!dirty || busy} on:click={resetOverrides}><RotateCcw size={16}/></button>
        <button class="primary-button action" disabled={!dirty || busy} on:click={saveOverrides}><Save size={16}/>{$t('Save','保存')}</button></div></div>{/if}
  {#if error}<div class="error-line" role="alert">{error}</div>{/if}

  {#if tab === 'general'}
    <section class="card"><div class="card-head"><h2>{$t('System','系统')}</h2></div><div class="card-body">
      {#each [['autostart','Start at sign-in','开机启动'],['closeToTray','Close to tray','关闭到托盘'],['autoSystemProxy','Set system proxy on connection','连接后设置系统代理']] as [key,en,zh]}<label class="setting-row"><span>{$t(en,zh)}</span><input type="checkbox" role="switch" checked={state.settings[key]} disabled={busy || (key === 'autoSystemProxy' && !!state.connection.pid)} on:change={(event) => run(() => runtime.updateSetting(key,event.currentTarget.checked))}/></label>{/each}
      <p class="subtle small">{$t('System proxy changes affect other applications on this computer. Disconnect before changing this preference.','系统代理会影响此计算机上的其他应用。请先断开连接再修改此选项。')}</p>
    </div></section>
    <section class="card"><div class="card-head"><h2>{$t('Recovery','恢复')}</h2></div><div class="card-body">
      <div class="setting-row"><span>{$t('System proxy recovery','系统代理恢复')}{#if state.proxyRecoveryPending} · <span class="warning">{$t('Pending','待处理')}</span>{/if}</span><button class="secondary-button action" disabled={busy} on:click={() => { if(confirm($t('Restore the saved system proxy settings?','恢复保存的系统代理设置？'))) run(() => runtime.restoreProxy(true)) }}><Undo2 size={16}/>{$t('Restore','恢复')}</button></div>
    </div></section>
    <section class="card"><div class="card-head"><h2>{$t('Appearance','外观')}</h2></div><div class="card-body">
      <label class="setting-row"><span>{$t('Theme','主题')}</span><select class="select-input" aria-label={$t('Theme','主题')} value={state.settings.appearance} on:change={(event) => run(() => runtime.updateSetting('appearance',event.currentTarget.value))}><option value="system">{$t('System','跟随系统')}</option><option value="light">{$t('Light','浅色')}</option><option value="dark">{$t('Dark','深色')}</option></select></label>
      <label class="setting-row"><span>{$t('Language','语言')}</span><select class="select-input" aria-label={$t('Language','语言')} value={state.settings.language} on:change={(event) => run(() => runtime.updateSetting('language',event.currentTarget.value))}><option>简体中文</option><option>English</option></select></label>
    </div></section>

  {:else if tab === 'kernel'}
    <section class="card"><div class="card-head"><h2>{$t('Kernel','内核')}</h2></div><div class="card-body">
      <div class="setting-row path"><label class="field grow"><span class="label">ppp.exe</span><input class="text-input mono" value={state.settings.pppPath} on:change={(event) => run(() => runtime.updateSetting('pppPath',event.currentTarget.value.trim()))}/></label><button class="icon-button" disabled={busy} title={$t('Browse','浏览')} aria-label={$t('Browse','浏览')} on:click={() => run(() => runtime.pickExecutable())}><FolderOpen size={17}/></button><button class="icon-button" disabled={busy} title={$t('Inspect kernel','检查内核')} aria-label={$t('Inspect kernel','检查内核')} on:click={() => run(() => runtime.inspectKernel())}><RefreshCw size={17}/></button></div>
      {#if state.settings.pppPath}<div class="setting-row"><span class="subtle">{$t('A custom kernel is selected. Clear it to use the ppp.exe shipped next to this client.','当前使用自定义内核路径。清空后将使用客户端同目录下自带的 ppp.exe。')}</span><button class="secondary-button action" disabled={busy || !!state.connection.pid} title={state.connection.pid ? $t('Disconnect first','请先断开连接') : ''} on:click={() => run(async () => { await runtime.updateSetting('pppPath',''); await runtime.inspectKernel() })}>{$t('Use bundled kernel','改用自带内核')}</button></div>{/if}
      {#if state.kernel}<div class="kernel-details"><strong>{state.kernel.version}</strong>{#each [['Statistics','统计',state.kernel.statsSupported],['Proxy','代理',state.kernel.proxySupported]] as [en,zh,supported]}<span>{$t(en,zh)}: {supported === true ? $t('Supported','支持') : supported === false ? $t('Not supported','不支持') : $t('Unknown','未知')}</span>{/each}</div>
      {:else}<p class="subtle">{$t('Capabilities not inspected. Without a custom path, the client looks next to its own executable.','尚未检查内核能力。未设置自定义路径时，客户端将在程序所在目录查找。')}</p>{/if}
      <div class="setting-row"><span>{$t('Privileges','运行权限')}: {state.administrator ? $t('Administrator','管理员') : $t('Standard user','普通用户')}<small class="subtle block">{$t('Adapter mode needs administrator rights.','网卡模式需要管理员权限。')}</small></span><button class="secondary-button action" disabled={busy || !!state.connection.pid || state.administrator} on:click={() => { if(confirm($t('Restart as administrator?','以管理员身份重启？'))) run(() => runtime.elevate()) }}><ShieldCheck size={16}/>{$t('Restart as administrator','以管理员身份重启')}</button></div>
    </div></section>

  {:else if tab === 'network'}
    <section class="card"><div class="card-head"><h2>{$t('Local proxy listeners','本地代理监听')}</h2><span class="subtle">{$t('Used in proxy mode and by the system proxy','代理模式和系统代理使用')}</span></div>
      <div class="card-body form-grid two">{#each listeners as [path,en,zh,type,fallback]}<label class="field"><span class="label">{$t(en,zh)}</span><input class="text-input mono" type={type === 'number' ? 'number' : 'text'} min="1" max="65535" value={get(path,fallback)} on:change={(event) => edit(path,type,event.currentTarget.value)}/></label>{/each}</div></section>
    <section class="card"><div class="card-head"><h2>P2P</h2></div><div class="card-body">
      <label class="setting-row"><span>{$t('Enable P2P','启用 P2P')}<small class="subtle block">{$t('Lets clients of the same server reach each other.','允许同一服务器下的客户端互相访问。')}</small></span><input type="checkbox" role="switch" checked={get('p2p/enabled',true) !== false} on:change={(event) => set('p2p/enabled',event.currentTarget.checked)}/></label>
      <label class="setting-row"><span>{$t('Data path','数据路径')}</span><select class="select-input wide-select" value={get('p2p/mode','relay')} disabled={get('p2p/enabled',true) === false} on:change={(event) => set('p2p/mode',event.currentTarget.value)}>{#each p2pModes as [value,en,zh]}<option {value}>{$t(en,zh)}</option>{/each}</select></label>
      <p class="subtle small">{$t('Listener and P2P edits use Save under Network overrides. Bind to loopback to restrict listeners to this computer.','监听与 P2P 修改由「网络覆盖」中的保存按钮保存。使用回环地址可将监听限制在本机。')}</p>
      <p class="subtle small">{$t('P2P direct on Windows has not been accepted on real NATs yet. The server must also allow direct-preferred; otherwise traffic stays on the relay.','Windows 上的 P2P 直连尚未经过真实 NAT 验收。服务器也需允许 direct-preferred，否则流量仍经服务器中转。')}</p>
    </div></section>
    {#if state.settings.connectionMode === 'proxy'}
    <section class="card"><div class="card-head"><h2>{$t('Proxy-mode access','代理模式访问范围')}</h2><span class="subtle">{$t('Saved automatically · next connection','自动保存 · 下次连接生效')}</span></div><div class="card-body">
      <label class="setting-row"><span>{$t('Join the virtual subnet in proxy mode','代理模式下接入虚拟子网')}<small class="subtle block">{$t('No adapter is created: the client joins inside its own process, and traffic to other clients through the local proxy goes P2P or via the server.','不创建网卡，在客户端进程内接入；经本地代理访问其他客户端，走 P2P 或经服务器中转。')}</small></span><input type="checkbox" role="switch" checked={state.launchOptions?.proxySubnet !== false} disabled={busy} on:change={(event) => run(() => runtime.updateLaunchOptions({...state.launchOptions,proxySubnet:event.currentTarget.checked}))}/></label>
      <label class="setting-row"><span>{$t('Proxy Internet in proxy mode','代理模式下代理上网')}<small class="subtle block">{$t('Off: Internet destinations are refused and the system proxy is not set.','关闭后拒绝访问互联网，也不设置系统代理。')}</small></span><input type="checkbox" role="switch" checked={state.launchOptions?.proxyInternet !== false} disabled={busy} on:change={(event) => run(() => runtime.updateLaunchOptions({...state.launchOptions,proxyInternet:event.currentTarget.checked}))}/></label>
      <label class="field"><span class="label">{$t('Ports open to other clients','开放给其他客户端的端口')}</span><input class="text-input mono" placeholder="22, 8080:3000" value={state.launchOptions?.proxyForwards || ''} disabled={busy || state.launchOptions?.proxySubnet === false} on:change={(event) => { const forwards=normalizeProxyForwards(event.currentTarget.value); if (forwards.error) { error=$t('Ports must be comma-separated port or port:localPort items (1-65535).','端口必须是逗号分隔的 端口 或 端口:本机端口（1-65535）。'); return } run(() => runtime.updateLaunchOptions({...state.launchOptions,proxyForwards:forwards.value})) }}/><small class="subtle">{$t('TCP only. "8080:3000" forwards port 8080 of your virtual IP to 127.0.0.1:3000; unlisted ports stay closed.','仅 TCP。"8080:3000" 表示把本机虚拟 IP 的 8080 端口转发到 127.0.0.1:3000；未列出的端口一律拒绝。')}</small></label>
    </div></section>
    {/if}
    <section class="card"><div class="card-body">{#if state.settings.connectionMode === 'proxy'}<p class="subtle small">{$t('In proxy mode, TUN IP, mask and gateway set the virtual-subnet address of this client; the other adapter options apply to adapter mode only.','代理模式下，TUN IP、子网掩码和网关决定本机在虚拟子网中的地址；其余选项只用于网卡模式。')}</p>{/if}<Config {state} {runtime} section="adapter" bind:dirty={configDirty}/></div></section>

  {:else}
    <section class="card"><div class="card-body"><Config {state} {runtime} section="advanced" bind:dirty={configDirty}/></div></section>
    <section class="card">
      <div class="card-head"><h2>{$t('Legacy routing','传统分流')}</h2><button class="inline-link accent" on:click={() => runtime.navigate('routing')}>{$t('Open Routing','前往分流页')}</button></div>
      <div class="card-body stack">
        <div class="notice" role="note">{policyOwnsRouting ? $t('A routing policy is in use; these settings do not apply.','正在使用分流策略，这里的设置不会生效。') : $t('Used only while no routing policy is enabled. The Routing page can migrate them into a policy.','仅在未启用分流策略时使用。可在「分流」页把它们迁移为策略。')}</div>
        <fieldset class="plain stack" disabled={policyOwnsRouting}>
          {#each legacyRouting as [path,en,zh,type,enHint,zhHint]}<label class="field"><span class="label">{$t(en,zh)}{#if enHint}<small class="subtle"> · {$t(enHint,zhHint)}</small>{/if}</span><textarea class="text-area short mono" value={display(path,type)} on:change={(event) => edit(path,type,event.currentTarget.value)} spellcheck="false"></textarea></label>{/each}
          <label class="field"><span class="label">GeoIP / GeoSite (JSON)</span><textarea class="text-area short mono" value={JSON.stringify(get('geo-rules',{}),null,2)} on:change={(event) => edit('geo-rules','json',event.currentTarget.value)} spellcheck="false"></textarea></label>
        </fieldset>
      </div>
    </section>
    <details class="card"><summary class="card-head"><h2>{$t('Network overrides JSON','网络覆盖 JSON')}</h2></summary><div class="card-body"><textarea class="text-area mono" bind:value={raw} on:change={applyRaw} on:input={() => error=''} spellcheck="false" aria-label={$t('Network overrides JSON','网络覆盖 JSON')}></textarea></div></details>
    <section class="card"><div class="card-head"><h2>{$t('Diagnostics','诊断')}</h2></div><div class="card-body stack">
      <p class="subtle small">{$t('Diagnostics include kernel, connection and event details. Review the file before sharing it.','诊断包含内核、连接和事件详情。分享前请检查文件内容。')}</p>
      <div class="toolbar"><button class="secondary-button action" on:click={inspect}><Eye size={16}/>{$t('Preview saved configuration','预览已保存配置')}</button><button class="secondary-button action" on:click={() => run(() => runtime.openData())}><FolderOpen size={16}/>{$t('Open data directory','打开数据目录')}</button><button class="secondary-button action" on:click={() => run(diagnostics)}><Download size={16}/>{$t('Export diagnostics','导出诊断')}</button></div>
      {#if preview}<pre class="preview">{JSON.stringify(preview,null,2)}</pre>{/if}
    </div></section>
  {/if}
  </div>
</div>

<style>
  .page,.tab-content { display:grid; gap:16px; min-width:0; } .tab-content > * { min-width:0; }
  .setting-row { display:flex; align-items:center; justify-content:space-between; gap:12px; min-height:56px; border-bottom:1px solid var(--border); }
  .setting-row:last-child { border:0; } .setting-row .select-input { width:160px; } .setting-row .wide-select { width:200px; }
  .setting-row.path { align-items:end; padding-bottom:12px; } .grow { flex:1; min-width:0; }
  .label { font-size:12px; color:var(--text-2); } .block { display:block; margin-top:3px; } .small { font-size:12px; margin:12px 0 0; }
  .kernel-details { display:flex; flex-wrap:wrap; gap:20px; padding:12px 0; color:var(--text-2); }
  .stack { display:grid; gap:14px; } .plain { border:0; margin:0; padding:0; min-width:0; } .short { min-height:120px; }
  .accent { color:var(--accent); }
  details.card > summary { cursor:pointer; list-style-position:inside; } details.card > summary h2 { display:inline; } details.card:not([open]) > summary { border-bottom:0; }
  .preview { overflow:auto; max-height:400px; padding:16px; margin:0; background:var(--surface); border-radius:8px; font-size:12px; }
  @media(max-width:760px) { .setting-row { flex-wrap:wrap; padding:12px 0; } }
</style>
