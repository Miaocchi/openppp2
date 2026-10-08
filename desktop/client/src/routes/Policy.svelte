<script>
  import { onMount } from 'svelte'
  import { Save, CheckCircle2, RotateCcw, Play, RefreshCw, Plus, Trash2 } from 'lucide-svelte'
  import { t } from '../lib/i18n.js'
  export let state
  export let runtime

  const tabs=[['rules','Rules','规则'],['settings','Settings','设置'],['test','Test','测试'],['create','Templates & migration','模板与迁移'],['status','Status','状态']]
  const templates=[['direct-all','Direct all','全部直连','Everything leaves directly; useful as a starting point for exceptions.','全部直连，适合在此基础上添加例外。'],['proxy-all','Proxy all','全部代理','Everything goes through the tunnel.','全部流量经隧道。'],['split-cn','Split CN','国内直连','China IP and sites go direct, the rest through the tunnel. Needs GeoIP and GeoSite sources.','国内 IP 与网站直连，其余经隧道。需要 GeoIP 与 GeoSite 来源。']]
  const ruleSetFormats=['geoip-text','geoip-dat','geosite-text','geosite-dat']

  let tab='rules', loading=true, busy='', error='', message=''
  let info={ enabled:false, policyDir:'', kernelSupported:null, kernelError:'' }
  let saved={ policy:null, rules:'' }
  let policy=null, rules='', raw=''
  let diagnostics=[], checkResult=null
  let target='', network='tcp', port='', explanation=null
  let geoip='', geosite='', migration=null
  let status=null, statusError='', updateResult=null

  onMount(load)

  async function load() {
    loading=true; error=''
    try {
      info=await runtime.policyLoad()
      saved={ policy:info.policy, rules:info.rules || '' }
      setDraft(info.policy, info.rules || '')
    } catch(cause) { error=String(cause) } finally { loading=false }
  }
  function setDraft(nextPolicy, nextRules) {
    policy=nextPolicy ? structuredClone(nextPolicy) : null
    rules=nextRules
    raw=policy ? JSON.stringify(policy,null,2) : ''
    diagnostics=[]; checkResult=null; explanation=null
  }
  // Structured fields and the raw JSON editor edit the same draft.
  function edit(mutate) { const next=structuredClone(policy || { version:2 }); mutate(next); policy=next; raw=JSON.stringify(policy,null,2); stale() }
  // Results describe the draft they were run on; drop them once it changes.
  function stale() { message=''; checkResult=null; diagnostics=[]; explanation=null }
  function setPath(path,value) { edit((next) => { const keys=path.split('/'); let node=next; for(const key of keys.slice(0,-1)) { if(!node[key] || typeof node[key] !== 'object') node[key]={}; node=node[key] } if(value === undefined) delete node[keys.at(-1)]; else node[keys.at(-1)]=value }) }
  // Takes the draft explicitly so Svelte re-renders fields when it changes.
  function read(source,path,fallback) { return path.split('/').reduce((value,key) => value?.[key],source) ?? fallback }
  function applyRaw() { try { const value=JSON.parse(raw); if(!value || Array.isArray(value) || typeof value !== 'object') throw new Error('client.policy must be a JSON object'); policy=value; error=''; stale() } catch(cause) { error=String(cause) } }

  async function run(label,task) { busy=label; error=''; message=''; try { return await task() } catch(cause) { error=String(cause) } finally { busy='' } }
  function showReport(result) { checkResult=result; diagnostics=result?.report?.diagnostics || [] }
  async function check() { await run('check',async () => { const result=await runtime.policyCheck(policy,rules); showReport(result); message=result.exitCode === 0 ? $t('Check passed','检查通过') : '' }) }
  async function save() {
    await run('save',async () => {
      const result=await runtime.policySave(policy,rules)
      showReport(result.result)
      if(result.result.exitCode === 0) { saved={ policy:structuredClone(policy), rules }; message=$t('Saved; applies on next connection','已保存，下次连接生效') }
      else tab='rules'
    })
  }
  async function toggleEnabled(event) {
    const wanted=event.currentTarget.checked
    await run('enable',async () => { info={ ...info, enabled:await runtime.policySetEnabled(wanted) } })
    if(info.enabled !== wanted) event.currentTarget.checked=info.enabled
  }
  async function explain() {
    await run('explain',async () => { const result=await runtime.policyExplain(policy,rules,target,network,Number(port) || null); explanation=result; diagnostics=result.report.diagnostics || [] })
  }
  async function fromTemplate(name) {
    if(dirty && !confirm($t('Replace the current draft?','替换当前草稿？'))) return
    await run('init',async () => {
      const result=await runtime.policyInit(name,geoip,geosite)
      if(result.policy) { setDraft(result.policy,result.rules); tab='rules'; message=$t('Template loaded; review and save','模板已载入，检查后保存') }
      else showReport(result.result)
    })
  }
  async function migrate() { await run('migrate',async () => { migration=await runtime.policyMigrate(); diagnostics=migration.result.report.diagnostics || [] }) }
  function adoptMigration() {
    if(dirty && !confirm($t('Replace the current draft?','替换当前草稿？'))) return
    setDraft(migration.policy,migration.rules); migration=null; tab='rules'; message=$t('Draft loaded; review the differences before saving','草稿已载入，保存前请核对差异')
  }
  async function refreshStatus() { statusError=''; await run('status',async () => { try { status=(await runtime.policyStatus()).report } catch(cause) { status=null; statusError=String(cause) } }) }
  async function updateNow() { await run('update',async () => { updateResult=await runtime.policyUpdate(); diagnostics=updateResult.report.diagnostics || []; await refreshStatus() }) }
  async function resetFakeIp() {
    if(!confirm($t('Clear all Fake-IP mappings? Applications that cached old answers must be restarted.','清除全部 Fake-IP 映射？缓存了旧解析结果的应用需要重启。'))) return
    await run('fakeip',async () => { message=await runtime.policyResetFakeIp() })
  }
  function addRuleSet() { setPath(`rule-sets/set${Object.keys(read(policy,'rule-sets',{})).length + 1}`,{ format:'geoip-dat', tag:'cn', source:{ path:'' } }) }
  function renameRuleSet(from,to) { to=to.trim(); if(!to || to === from) return; edit((next) => { const sets=next['rule-sets'] || {}; if(sets[to]) return; next['rule-sets']=Object.fromEntries(Object.entries(sets).map(([key,value]) => [key === from ? to : key,value])) }) }
  function setRuleSetSource(name,kind,value) { setPath(`rule-sets/${name}/source`,{ [kind]:value }) }
  function time(ms) { return ms > 0 ? new Date(ms).toLocaleString() : '—' }

  $: dirty=JSON.stringify(policy) !== JSON.stringify(saved.policy) || rules !== saved.rules
  $: ruleSets=Object.entries(read(policy,'rule-sets',{}) || {})
  $: unsupported=info.kernelSupported === false || (info.kernelSupported === null && info.kernelError)
  $: connected=['connected'].includes(state.connection.status)
  $: action=explanation?.report?.route
  $: dns=explanation?.report?.dns
</script>

<div class="page"><div class="page-heading"><h1>{$t('Policy','策略')}</h1><div class="toolbar"><span class="subtle">{busy ? $t('Working…','处理中…') : dirty ? $t('Unsaved changes','未保存') : message}</span><button class="icon-button" title={$t('Discard changes','放弃修改')} aria-label={$t('Discard changes','放弃修改')} disabled={!dirty || !!busy} on:click={() => setDraft(saved.policy,saved.rules)}><RotateCcw size={16}/></button><button class="secondary-button action" disabled={!policy || !!busy || unsupported} on:click={check}><CheckCircle2 size={16}/>{$t('Check','检查')}</button><button class="primary-button action" disabled={!policy || !dirty || !!busy || unsupported} on:click={save}><Save size={16}/>{$t('Save','保存')}</button></div></div>
  {#if unsupported}<div class="notice" role="alert">{info.kernelError || $t('The selected kernel does not support policy v2 (requires 2.1.7 or later).','所选内核不支持策略 v2（需要 2.1.7 或更高版本）。')}</div>{/if}
  {#if error}<div class="error-line" role="alert">{error}</div>{/if}
  <label class="option-row"><span><strong>{$t('Use this policy for all connections','所有连接使用此策略')}</strong><small class="subtle">{$t('Replaces node-provided policies and the Routing/DNS settings on the Network page.','替代节点自带策略，以及网络页的分流、DNS 设置。')}</small></span><input type="checkbox" role="switch" checked={info.enabled} disabled={!saved.policy || !!busy} on:change={toggleEnabled}/></label>

  <div class="tabs" role="tablist" aria-label={$t('Policy sections','策略分区')}>{#each tabs as [id,en,zh]}<button role="tab" aria-selected={tab === id} class:active={tab === id} on:click={() => tab=id}>{$t(en,zh)}</button>{/each}</div>

  {#if loading}<div class="empty">{$t('Loading…','加载中…')}</div>
  {:else if !policy && tab !== 'create' && tab !== 'status'}
    <div class="empty"><div class="center"><p>{$t('No policy yet. Start from a template or migrate your current Network settings.','还没有策略。可从模板创建，或迁移网络页的现有设置。')}</p><button class="primary-button action" on:click={() => tab='create'}><Plus size={16}/>{$t('Create policy','创建策略')}</button></div></div>
  {:else if tab === 'rules'}
    <section class="band">
      <textarea class="text-area mono rules" bind:value={rules} on:input={stale} spellcheck="false" aria-label={$t('Rule file','规则文件')}></textarea>
      <details class="hint"><summary>{$t('Rule syntax','规则语法')}</summary><pre>{`default proxy            # exactly one: direct | proxy | reject
dns direct local          # resolver for direct names
dns proxy remote          # resolver for proxied names
[direct]
=exact.example            # exact host
example.com               # domain and subdomains
*.example.org             # subdomains only
keyword:ads               # host contains "ads"
regexp:^cdn[0-9]+\\.       # regular expression
10.0.0.0/8                # IPv4 CIDR
set:geoip-cn              # rule set declared in Settings
[reject]
[dns:local]               # names resolved by a specific resolver`}</pre></details>
    </section>
  {:else if tab === 'settings'}
    <section class="band"><div class="form-grid two">
      <label class="field"><span>{$t('DNS mode','DNS 模式')}</span><select class="select-input" value={read(policy,'dns/mode','auto')} on:change={(e) => setPath('dns/mode',e.currentTarget.value)}><option value="auto">{$t('Auto (Fake-IP on TUN)','自动（TUN 下 Fake-IP）')}</option><option value="real">{$t('Real IP','真实 IP')}</option><option value="fake-ip">Fake-IP</option></select></label>
      <label class="field"><span>{$t('Fake-IP range','Fake-IP 地址池')}</span><input class="text-input mono" value={read(policy,'dns/fake-ip/range','198.18.0.0/16')} on:change={(e) => setPath('dns/fake-ip/range',e.currentTarget.value.trim())}/></label>
      <label class="option-row"><span>{$t('Block IPv6','阻断 IPv6')}<small class="subtle">{$t('Prevents IPv6 leaks around the tunnel','防止 IPv6 绕过隧道泄漏')}</small></span><input type="checkbox" role="switch" checked={read(policy,'ipv6','') === 'block'} on:change={(e) => setPath('ipv6',e.currentTarget.checked ? 'block' : undefined)}/></label>
      <label class="option-row"><span>{$t('TCP domain sniffing','TCP 域名嗅探')}<small class="subtle">{$t('TUN only: match HTTPS/HTTP by SNI/Host','仅 TUN：按 SNI/Host 匹配域名')}</small></span><input type="checkbox" role="switch" checked={!!read(policy,'tcp-domain-sniff',false)} on:change={(e) => setPath('tcp-domain-sniff',e.currentTarget.checked)}/></label>
    </div>
    <div class="notice spaced">{$t('Changing the Fake-IP range or storage needs a reconnect and clearing DNS caches; use Status → Reset Fake-IP.','修改 Fake-IP 地址池或存储需要重新连接并清理 DNS 缓存，可在“状态”页重置 Fake-IP。')}</div></section>
    <section class="band"><div class="section-heading"><h2>{$t('Rule sets','规则集')}</h2><button class="secondary-button action" on:click={addRuleSet}><Plus size={15}/>{$t('Add','添加')}</button></div>
      {#if ruleSets.length === 0}<p class="subtle">{$t('No rule sets. Reference them in rules as set:NAME.','暂无规则集。在规则中用 set:名称 引用。')}</p>{/if}
      {#each ruleSets as [name,set] (name)}<div class="rule-set">
        <input class="text-input mono" aria-label={$t('Name','名称')} value={name} on:change={(e) => renameRuleSet(name,e.currentTarget.value)}/>
        <select class="select-input" aria-label={$t('Format','格式')} value={set.format} on:change={(e) => setPath(`rule-sets/${name}/format`,e.currentTarget.value)}>{#each ruleSetFormats as format}<option>{format}</option>{/each}</select>
        <input class="text-input mono" aria-label="Tag" placeholder="tag" value={set.tag || ''} on:change={(e) => setPath(`rule-sets/${name}/tag`,e.currentTarget.value.trim() || undefined)}/>
        <input class="text-input mono wide" aria-label={$t('Path or URL','路径或 URL')} placeholder={$t('Path relative to the policy folder, or https:// URL','相对策略目录的路径，或 https:// 地址')} value={set.source?.url || set.source?.path || ''} on:change={(e) => { const value=e.currentTarget.value.trim(); setRuleSetSource(name,/^https?:\/\//i.test(value) ? 'url' : 'path',value) }}/>
        <button class="icon-button" title={$t('Remove','删除')} aria-label={$t('Remove','删除')} on:click={() => setPath(`rule-sets/${name}`,undefined)}><Trash2 size={15}/></button>
      </div>{/each}
      {#if info.policyDir}<p class="subtle">{$t('Relative paths resolve under','相对路径按此目录解析：')} <code>{info.policyDir}</code></p>{/if}
    </section>
    <section class="band"><h2>{$t('Automatic updates','自动更新')}</h2><div class="form-grid two">
      <label class="option-row"><span>{$t('Update remote rule sets while connected','连接期间自动更新远程规则集')}</span><input type="checkbox" role="switch" checked={!!read(policy,'updates/enabled',false)} on:change={(e) => setPath('updates/enabled',e.currentTarget.checked)}/></label>
      <label class="field"><span>{$t('Interval','间隔')}</span><input class="text-input mono" value={read(policy,'updates/interval','24h')} on:change={(e) => setPath('updates/interval',e.currentTarget.value.trim())}/></label>
      <label class="field"><span>{$t('Download through','下载经由')}</span><select class="select-input" value={read(policy,'updates/via','proxy')} on:change={(e) => setPath('updates/via',e.currentTarget.value)}><option value="proxy">{$t('Tunnel','隧道')}</option><option value="direct">{$t('Direct','直连')}</option></select></label>
    </div></section>
    <details class="band"><summary>{$t('client.policy JSON','client.policy JSON')}</summary><textarea class="text-area mono spaced" bind:value={raw} on:change={applyRaw} spellcheck="false" aria-label="client.policy JSON"></textarea></details>
  {:else if tab === 'test'}
    <section class="band"><p class="subtle">{$t('Evaluates the current draft offline; no DNS lookup or connection is made.','离线评估当前草稿，不做 DNS 查询，也不建立连接。')}</p>
      <form class="test-row" on:submit|preventDefault={explain}>
        <input class="text-input mono" placeholder={$t('Domain or IPv4, e.g. www.example.com','域名或 IPv4，如 www.example.com')} bind:value={target} aria-label={$t('Target','目标')}/>
        <select class="select-input" bind:value={network} aria-label={$t('Protocol','协议')}><option value="tcp">TCP</option><option value="udp">UDP</option></select>
        <input class="text-input" type="number" min="1" max="65535" placeholder={$t('Port','端口')} bind:value={port} aria-label={$t('Port','端口')}/>
        <button class="primary-button action" disabled={!target.trim() || !!busy || unsupported}><Play size={15}/>{$t('Test','测试')}</button>
      </form>
      {#if action}<div class="verdict">
        <div><span class="subtle">{$t('Route','路由')}</span><strong class={`action-${action.action}`}>{({direct:$t('Direct','直连'),proxy:$t('Via tunnel','经隧道'),reject:$t('Reject','拒绝')})[action.action] || action.action}</strong><small>{action.matched ? `${action.rule_id} · ${action.file}:${action.line}` : $t('No rule matched; default action','未命中规则，使用默认动作')}{action.needs_ip ? ` · ${$t('may change after IP resolution','解析 IP 后可能变化')}` : ''}</small></div>
        {#if dns?.applicable}<div><span class="subtle">DNS</span><strong>{dns.rejected ? $t('Refused','拒绝解析') : dns.resolver}</strong><small>{$t('via','经')} {({direct:$t('direct','直连'),proxy:$t('tunnel','隧道')})[dns.via] || dns.via} · {dns.rule_id}{dns.line ? ` · ${dns.file}:${dns.line}` : ''}</small></div>{/if}
      </div>{/if}
    </section>
  {:else if tab === 'create'}
    <section class="band"><h2>{$t('Start from a template','从模板开始')}</h2>
      <div class="templates">{#each templates as [name,en,zh,enHint,zhHint]}<div class="template"><strong>{$t(en,zh)}</strong><small class="subtle">{$t(enHint,zhHint)}</small>
        {#if name === 'split-cn'}<input class="text-input mono" placeholder={$t('GeoIP: file path or https:// URL','GeoIP：文件路径或 https:// 地址')} bind:value={geoip} aria-label="GeoIP"/><input class="text-input mono" placeholder={$t('GeoSite: file path or https:// URL','GeoSite：文件路径或 https:// 地址')} bind:value={geosite} aria-label="GeoSite"/>{/if}
        <button class="secondary-button action" disabled={!!busy || unsupported || (name === 'split-cn' && (!geoip.trim() || !geosite.trim()))} on:click={() => fromTemplate(name)}>{$t('Use','使用')}</button></div>{/each}</div>
    </section>
    <section class="band"><div class="section-heading"><h2>{$t('Migrate Network settings','迁移网络页设置')}</h2><button class="secondary-button action" disabled={!!busy || unsupported} on:click={migrate}><RefreshCw size={15}/>{$t('Create draft','生成草稿')}</button></div>
      <p class="subtle">{$t('Converts the base configuration and Network page overrides into a v2 draft. Nothing changes until you save.','把基础配置与网络页覆盖项转换为 v2 草稿，保存前不会生效。')}</p>
      {#if migration}
        {@const report=migration.result.report.migration || {}}
        <div class={migration.result.exitCode === 0 ? 'notice ok' : 'notice'}>{migration.result.exitCode === 0 ? $t('Routing is equivalent to the current settings.','分流结果与当前设置等价。') : migration.result.exitCode === 5 ? $t('Draft differs from the current settings; review each item below before adopting it.','草稿与当前设置存在差异，采用前请逐项核对。') : $t('Migration failed.','迁移失败。')}</div>
        {#if report.diagnostics?.length}<ul class="diagnostics">{#each report.diagnostics as item}<li><code>{item.code}</code> {item.message}</li>{/each}</ul>{/if}
        {#if migration.policy}<button class="primary-button action" on:click={adoptMigration}>{$t('Load draft into editor','载入草稿到编辑器')}</button>{/if}
      {/if}
    </section>
  {:else if tab === 'status'}
    <section class="band"><div class="section-heading"><h2>{$t('Runtime status','运行状态')}</h2><div class="toolbar"><button class="secondary-button action" disabled={!!busy || unsupported} on:click={refreshStatus}><RefreshCw size={15}/>{$t('Refresh','刷新')}</button><button class="secondary-button action" disabled={!!busy || unsupported || !connected} title={connected ? '' : $t('Connect first','请先连接')} on:click={updateNow}>{$t('Update rule sets now','立即更新规则集')}</button></div></div>
      {#if statusError}<p class="subtle">{statusError}</p>{/if}
      {#if status}<dl class="status-grid">
        <dt>{$t('State','状态')}</dt><dd>{status.runtime_state}{status.reason ? ` · ${status.reason}` : ''}</dd>
        <dt>{$t('Active version','生效版本')}</dt><dd class="mono">{status.active_version ?? '—'}</dd>
        <dt>{$t('Prepared version','待生效版本')}</dt><dd class="mono">{status.prepared_version || '—'}{status.prepared_pending_commit ? ` · ${$t('applies on next connection','下次连接生效')}` : ''}</dd>
        <dt>{$t('Last update','上次更新')}</dt><dd>{time(status.last_attempt_ms)} · {status.last_result || '—'}</dd>
        <dt>{$t('Last success','上次成功')}</dt><dd>{time(status.last_success_ms)}</dd>
        <dt>{$t('Next attempt','下次尝试')}</dt><dd>{time(status.next_attempt_ms)}</dd>
        {#if status.last_diagnostic}<dt>{$t('Last error','最近错误')}</dt><dd class="warning">{status.last_diagnostic}</dd>{/if}
      </dl>
      {#if status.sources?.length}<table class="sources"><thead><tr><th>{$t('Source','来源')}</th><th>URL</th><th>{$t('Size','大小')}</th><th>{$t('Validated','校验时间')}</th></tr></thead><tbody>{#each status.sources as source}<tr><td>{source.name}</td><td class="mono">{source.url_redacted}</td><td>{source.bytes}</td><td>{time(source.validated_at_ms)}</td></tr>{/each}</tbody></table>{/if}{/if}
      {#if updateResult}<p class="subtle">{$t('Update result','更新结果')}: {updateResult.report.update_state || updateResult.report.status}</p>{/if}
    </section>
    <section class="band"><div class="section-heading"><h2>Fake-IP</h2><button class="secondary-button action" disabled={!!busy || state.connection.pid} on:click={resetFakeIp}><Trash2 size={15}/>{$t('Reset Fake-IP','重置 Fake-IP')}</button></div>
      <p class="subtle">{$t('Clears stored name-to-address mappings. Only while disconnected; afterwards restart apps that cached DNS answers. Windows DNS cache is flushed automatically.','清除已保存的域名与地址映射。仅在断开时可用；之后需重启缓存了 DNS 结果的应用。Windows 会自动刷新系统 DNS 缓存。')}</p>
    </section>
  {/if}

  {#if diagnostics.length}<section class="band" aria-live="polite"><h2>{$t('Diagnostics','诊断')}</h2><ul class="diagnostics">{#each diagnostics as item}<li class:warning={item.severity === 'warning'}><code>{item.code}</code> {item.message}{#if item.source}<span class="subtle"> — {item.source}{item.line ? `:${item.line}` : ''}</span>{/if}</li>{/each}</ul></section>
  {:else if checkResult?.exitCode === 0}<p class="ok-line">{$t('Check passed','检查通过')} · {$t('rules','规则')} {checkResult.report.rule_count ?? '—'}</p>{/if}
</div>

<style>
  .rules { min-height:360px; } code { overflow-wrap:anywhere; } .spaced { margin:16px 0; } .center { display:grid; gap:14px; justify-items:center; text-align:center; }
  .hint summary, details > summary { cursor:pointer; color:var(--text-2); padding:10px 0; } .hint pre { font-size:12px; color:var(--text-2); overflow:auto; }
  .option-row { min-height:52px; display:flex; align-items:center; justify-content:space-between; gap:16px; border-bottom:1px solid var(--border); } .option-row span { display:grid; gap:4px; }
  .rule-set { display:grid; grid-template-columns:minmax(90px,1fr) 140px 90px minmax(160px,3fr) auto; gap:8px; margin:10px 0; }
  .test-row { display:grid; grid-template-columns:minmax(0,1fr) 90px 110px auto; gap:8px; margin:12px 0; }
  .verdict { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:18px; margin-top:16px; } .verdict div { display:grid; gap:6px; } .verdict strong { font-size:20px; }
  .action-direct { color:var(--green, #3fb950); } .action-proxy { color:var(--accent); } .action-reject { color:var(--red); }
  .templates { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:16px; margin-top:12px; } .template { display:grid; gap:8px; align-content:start; padding:14px; border:1px solid var(--border); border-radius:8px; }
  .diagnostics { margin:8px 0; padding-left:18px; display:grid; gap:6px; font-size:13px; } .ok-line { color:var(--green, #3fb950); font-size:13px; } .notice.ok { border-color:rgba(63,185,80,.35); background:rgba(63,185,80,.08); color:var(--text); }
  .status-grid { display:grid; grid-template-columns:max-content minmax(0,1fr); gap:8px 18px; margin:14px 0; } .status-grid dt { color:var(--text-3); } .status-grid dd { margin:0; overflow-wrap:anywhere; }
  .sources { width:100%; border-collapse:collapse; font-size:12px; } .sources th, .sources td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--border); overflow-wrap:anywhere; }
  @media(max-width:760px) { .rule-set, .test-row, .templates, .verdict { grid-template-columns:1fr; } .form-grid.two { grid-template-columns:1fr; } }
</style>
