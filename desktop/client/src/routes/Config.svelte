<script>
  import { Save, Check, RotateCcw, Plus, Trash2 } from 'lucide-svelte'
  import { t } from '../lib/i18n.js'
  import { readMultipathConfig, validateMultipathConfig, writeMultipathConfig } from '../lib/multipathConfig.js'
  export let state
  export let runtime
  export let section='all'
  // Only edited keys are kept locally, so options changed elsewhere (e.g. Settings switches) are never overwritten.
  let draft=state.config, edits={}, message='', error='', saving=false
  function reset() { draft=state.config; edits={}; message=''; error='' }
  function validate() { const value=JSON.parse(draft); if(!value || Array.isArray(value) || typeof value !== 'object') throw new Error($t('Expected JSON object','必须是 JSON 对象')); validateMultipathConfig(value); return value }
  async function save() { saving=true; error=''; try { const value=validate(); await runtime.updateClientConfig(JSON.stringify(value,null,2),options); edits={}; message=$t('Saved; applies on next connection','已保存，下次连接生效') } catch(cause) { error=String(cause) } finally { saving=false } }
  function check() { try { validate(); message=$t('Valid JSON','JSON 格式有效'); error='' } catch(cause) { error=String(cause) } }
  function updateMultipath(change) {
    try {
      const root=JSON.parse(draft)
      if (!root || Array.isArray(root) || typeof root !== 'object') throw new Error($t('Expected JSON object','必须是 JSON 对象'))
      const current=readMultipathConfig(root)
      const next=change(structuredClone(current))
      draft=JSON.stringify(writeMultipathConfig(root,next),null,2)
      message=''; error=''
    } catch(cause) { error=String(cause) }
  }
  function setMultipathField(key,value) { updateMultipath((mp) => ({...mp,[key]:value})) }
  function setPathField(index,key,value) {
    updateMultipath((mp) => ({...mp,paths:mp.paths.map((path,i) => i === index ? {...path,[key]:value} : path)}))
  }
  function setPathTransport(index,transport) {
    updateMultipath((mp) => ({...mp,paths:mp.paths.map((path,i) => {
      if (i !== index) return path
      const next={...path,transport}
      if (transport === 'tcp') { next.tls_server_name=''; next.http_host=''; next.ws_path='' }
      else if (transport === 'ws') next.tls_server_name=''
      return next
    })}))
  }
  function addPath() {
    updateMultipath((mp) => {
      if (mp.paths.length >= 8) throw new Error($t('At most 8 paths are supported','最多支持 8 条路径'))
      const ids=new Set(mp.paths.map((path) => path.id))
      let n=1; while (ids.has(`path-${n}`)) n++
      return {...mp,paths:[...mp.paths,{id:`path-${n}`,transport:'tcp',connect_host:'example.invalid',port:443,connections:1,tls_server_name:'',http_host:'',ws_path:''}]}
    })
  }
  function removePath(index) { updateMultipath((mp) => ({...mp,paths:mp.paths.filter((_,i) => i !== index)})) }
  $: options={...state.launchOptions,...edits}
  $: parsedDraft=(() => { try { return JSON.parse(draft) } catch { return {} } })()
  $: multipath=readMultipathConfig(parsedDraft)
  $: dirty=draft !== state.config || Object.keys(edits).some((key) => edits[key] !== state.launchOptions?.[key])
</script>
<section><div class="section-heading"><h2>{$t(section === 'adapter' ? 'Virtual adapter' : 'Launch options',section === 'adapter' ? '虚拟网卡' : '启动参数')}</h2><div class="toolbar"><span class="subtle">{dirty ? $t('Unsaved changes','未保存') : message}</span><button class="icon-button" title={$t('Validate','校验')} aria-label={$t('Validate','校验')} on:click={check}><Check size={15}/></button><button class="icon-button" title={$t('Reset','恢复')} aria-label={$t('Reset','恢复')} on:click={reset}><RotateCcw size={15}/></button><button class="primary-button action" disabled={saving || !dirty} on:click={save}><Save size={15}/>{$t('Save','保存')}</button></div></div>
  {#if section !== 'advanced'}<div class="form-grid two spaced">{#each [['tunIp','TUN IP','TUN IP'],['tunMask','Mask','子网掩码'],['gateway','Gateway','网关']] as [key,en,zh]}<label class="field"><span>{$t(en,zh)}</span><input class="text-input mono" value={options[key] || ''} on:input={(event) => edits={...edits,[key]:event.currentTarget.value}}/></label>{/each}
    {#if state.kernel?.tcpStackSupported !== false}<label class="field"><span>{$t('TCP stack','TCP 协议栈')}</span><select class="select-input" value={options.tcpStack || ''} on:change={(event) => edits={...edits,tcpStack:event.currentTarget.value}}><option value="">{$t('Kernel default','内核默认')}</option><option value="native">native</option><option value="lwip">lwIP</option><option value="xtcp">{$t('xtcp (experimental, must be built in)','xtcp（实验，需内核编译支持）')}</option></select></label>{/if}
    {#if state.kernel?.tunIpv6Supported !== false}<label class="field"><span>{$t('Requested IPv6 (optional)','请求的 IPv6（可选）')}</span><input class="text-input mono" placeholder={$t('Server assigns when empty','留空由服务器分配')} value={options.tunIpv6 || ''} on:input={(event) => edits={...edits,tunIpv6:event.currentTarget.value}}/></label>{/if}</div>{/if}
  {#if section !== 'adapter'}<div class="form-grid two spaced"><label class="field"><span>{$t('MUX links','MUX 数量')}</span><input class="text-input" type="number" min="0" max="65535" value={options.mux || 0} on:input={(event) => edits={...edits,mux:Number(event.currentTarget.value)}}/></label><label class="field"><span>{$t('MUX mode','MUX 模式')}</span><select class="select-input" value={options.muxMode || 'compat'} on:change={(event) => edits={...edits,muxMode:event.currentTarget.value}}><option>compat</option><option>flow</option><option>balance</option><option value="stripe">{$t('stripe (experimental)','stripe（实验）')}</option></select></label></div>
    {#each [['muxTurbo','Flow Turbo'],['vnet','VNet'],['blockQuic','Block QUIC'],['staticMode','Static UDP']] as [key,label]}<label class="option-row"><span>{label}</span><input type="checkbox" role="switch" checked={!!options[key]} disabled={key === 'muxTurbo' && options.muxMode !== 'flow'} on:change={(event) => edits={...edits,[key]:event.currentTarget.checked}}/></label>{/each}
    <div class="multipath spaced">
      <div class="subhead"><h3>{$t('Multipath','多路径')}</h3><label class="option-row compact"><span>{$t('Enable multipath','启用多路径')}</span><input type="checkbox" role="switch" checked={!!multipath.enabled} on:change={(event) => setMultipathField('enabled',event.currentTarget.checked)}/></label></div>
      <div class="form-grid two spaced">
        <label class="field"><span>{$t('Scheduling policy','调度策略')}</span><select class="select-input" value={multipath.policy || 'adaptive'} on:change={(event) => setMultipathField('policy',event.currentTarget.value)}><option value="adaptive">adaptive</option><option value="failover">failover</option></select></label>
        <label class="field"><span>{$t('Maximum connections','最大连接数')}</span><input class="text-input" type="number" min="1" max="32" value={multipath.max_connections ?? 8} on:change={(event) => setMultipathField('max_connections',Number(event.currentTarget.value))}/></label>
        <label class="field"><span>{$t('Concurrent connection attempts','并发建连数')}</span><input class="text-input" type="number" min="1" max="8" value={multipath.max_concurrent_connects ?? 4} on:change={(event) => setMultipathField('max_concurrent_connects',Number(event.currentTarget.value))}/></label>
        <label class="field"><span>{$t('Recovery grace (ms)','恢复宽限期（毫秒）')}</span><input class="text-input" type="number" min="1000" max="30000" step="1000" value={multipath.resume_grace_ms ?? 30000} on:change={(event) => setMultipathField('resume_grace_ms',Number(event.currentTarget.value))}/></label>
      </div>
      <div class="path-heading"><span>{$t('Paths','路径')} · {multipath.paths?.length || 0}/8</span><button class="icon-button" title={$t('Add path','添加路径')} aria-label={$t('Add path','添加路径')} disabled={(multipath.paths?.length || 0) >= 8} on:click={addPath}><Plus size={16}/></button></div>
      {#each multipath.paths || [] as path,index}
        <div class="path-editor">
          <div class="path-title"><strong>{path.id || $t('Path','路径')} {index + 1}</strong><button class="icon-button danger" title={$t('Remove path','删除路径')} aria-label={$t('Remove path','删除路径')} on:click={() => removePath(index)}><Trash2 size={15}/></button></div>
          <div class="form-grid two">
            <label class="field"><span>{$t('Path ID','路径 ID')}</span><input class="text-input mono" maxlength="64" value={path.id || ''} on:input={(event) => setPathField(index,'id',event.currentTarget.value)}/></label>
            <label class="field"><span>{$t('Transport','传输方式')}</span><select class="select-input" value={path.transport || 'tcp'} on:change={(event) => setPathTransport(index,event.currentTarget.value)}><option value="tcp">TCP</option><option value="ws">WS</option><option value="wss">WSS</option></select></label>
            <label class="field"><span>{$t('Connect host','连接主机')}</span><input class="text-input mono" value={path.connect_host || ''} on:input={(event) => setPathField(index,'connect_host',event.currentTarget.value)}/></label>
            <label class="field"><span>{$t('Port','端口')}</span><input class="text-input" type="number" min="1" max="65535" value={path.port ?? 443} on:change={(event) => setPathField(index,'port',Number(event.currentTarget.value))}/></label>
            <label class="field"><span>{$t('Connections','连接数')}</span><input class="text-input" type="number" min="1" max="32" value={path.connections ?? 1} on:change={(event) => setPathField(index,'connections',Number(event.currentTarget.value))}/></label>
            {#if path.transport === 'wss'}<label class="field"><span>{$t('TLS server name','TLS 服务器名称')}</span><input class="text-input mono" value={path.tls_server_name || ''} on:input={(event) => setPathField(index,'tls_server_name',event.currentTarget.value)}/></label>{/if}
            {#if path.transport === 'ws' || path.transport === 'wss'}<label class="field"><span>{$t('HTTP Host','HTTP Host')}</span><input class="text-input mono" value={path.http_host || ''} on:input={(event) => setPathField(index,'http_host',event.currentTarget.value)}/></label><label class="field"><span>{$t('WebSocket path','WebSocket 路径')}</span><input class="text-input mono" value={path.ws_path || ''} on:input={(event) => setPathField(index,'ws_path',event.currentTarget.value)}/></label>{/if}
          </div>
        </div>
      {/each}
    </div>
    <details class="spaced"><summary>{$t('Advanced appsettings JSON','高级 appsettings JSON')}</summary><textarea class="text-area spaced" bind:value={draft} spellcheck="false" aria-label="appsettings JSON"></textarea></details>{/if}
  {#if error}<div class="error-line" role="alert">{error}</div>{/if}
</section>
<style>.spaced { margin:20px 0; } summary { cursor:pointer; color:var(--text-2); } .option-row { min-height:46px; display:flex; align-items:center; justify-content:space-between; border-bottom:1px solid var(--border); } .multipath { border-top:1px solid var(--border); padding-top:18px; } .subhead,.path-heading,.path-title { display:flex; align-items:center; justify-content:space-between; gap:12px; } h3 { margin:0; font-size:15px; } .compact { min-height:36px; gap:16px; border:0; } .path-heading { margin:16px 0 8px; font-size:12px; color:var(--text-2); } .path-editor { padding:12px 0 16px; border-top:1px solid var(--border); } .path-title { margin-bottom:12px; } .danger { color:var(--danger); }</style>
