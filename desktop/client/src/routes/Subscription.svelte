<script>
  import { Plus, RefreshCw, Trash2, Pencil, Save } from 'lucide-svelte'
  import { t } from '../lib/i18n.js'
  import { formatDateTime } from '../lib/format.js'
  import { tick } from 'svelte'
  import { redactText } from '../lib/security.js'
  export let state
  export let runtime
  let editing=null, replacement='', formError='', failures={}, busy='', nameField, opener
  $: sources=state.subscription.sources || []
  $: enabled=sources.filter((source) => source.enabled)

  function safe(text) { return redactText(text,[...sources.map((source) => source.url),replacement]) }
  async function edit(source,event) {
    opener=event.currentTarget
    editing=source ? {...source,name:safe(source.name)} : {id:'',name:'',enabled:true}
    replacement=''; formError=''
    await tick(); nameField?.focus()
  }
  async function close() { editing=null; replacement=''; formError=''; await tick(); opener?.focus() }
  async function save() {
    if(busy) return
    busy='editor'; formError=''
    try {
      const url=replacement.trim() || editing.url
      if(!/^https?:\/\//i.test(url || '')) throw new Error($t('Use an HTTP or HTTPS address.','请输入 HTTP 或 HTTPS 地址。'))
      await runtime.saveSubscription({...editing,name:editing.name.trim(),url})
      failures={...failures,[editing.id]:''}; busy=''; await close()
    } catch(cause) { formError=safe(cause) } finally { busy='' }
  }
  async function perform(source,action) {
    busy=source.id; failures={...failures,[source.id]:''}
    try { await action() } catch(cause) { failures={...failures,[source.id]:safe(cause)} } finally { busy='' }
  }
  function refresh(source) { return perform(source,() => runtime.refreshSubscription(source.url,source.id)) }
  async function toggle(source,input) {
    const enabled=input.checked
    await perform(source,() => runtime.saveSubscription({...source,enabled}))
    if(failures[source.id]) input.checked=source.enabled
  }
  async function refreshAll() { for(const source of enabled) await refresh(source) }
  function remove(source) {
    if(confirm($t('Delete this source and its subscription nodes?','删除此来源及其订阅节点？'))) perform(source,() => runtime.deleteSubscription(source.id))
  }
</script>
<div class="page">
  <div class="page-heading"><div><h1>{$t('Subscriptions','订阅')}</h1><p class="page-lead">{$t('Refresh a source to update its nodes. Local nodes are managed separately.','更新来源即可同步其中的节点。本地节点单独管理。')}</p></div>
    <div class="toolbar"><button class="secondary-button action" disabled={!!busy || !enabled.length || !!editing} on:click={refreshAll}><RefreshCw size={15}/>{$t('Refresh all','全部更新')}</button><button class="primary-button action" disabled={!!busy || !!editing} on:click={(e) => edit(null,e)}><Plus size={16}/>{$t('Add source','添加来源')}</button></div>
  </div>
  {#if editing}
    <form class="card" on:submit|preventDefault={save}>
      <div class="card-head"><h2>{editing.id ? $t('Edit source','编辑来源') : $t('New source','新来源')}</h2></div>
      <div class="card-body editor"><label class="field"><span>{$t('Name','名称')}</span><input class="text-input" bind:this={nameField} bind:value={editing.name} required/></label>
        <label class="field"><span>URL</span><input class="text-input" type="password" autocomplete="off" spellcheck="false" bind:value={replacement} required={!editing.id} aria-describedby="url-help" placeholder="https://"/></label>
        <p class="subtle" id="url-help">{editing.id ? $t('Leave blank to keep the stored address. Enter a URL only to replace it.','留空保留原地址。仅在更换地址时输入新 URL。') : $t('Subscription links may contain credentials. Use a descriptive name, not the link itself.','订阅链接可能包含凭据。请使用易辨认的名称，不要将链接用作名称。')}</p>
        {#if formError}<div role="alert" class="error-line">{formError}</div>{/if}
        <div class="toolbar"><button class="primary-button action" disabled={!!busy}><Save size={15}/>{$t('Save','保存')}</button><button class="secondary-button" type="button" disabled={!!busy} on:click={close}>{$t('Cancel','取消')}</button></div>
      </div>
    </form>
  {/if}
  <section class="card"><div class="card-head"><h2>{$t('Sources','来源')} · {sources.length}</h2><span class="subtle">{enabled.length} {$t('enabled','个已启用')}</span></div>
    {#each sources as source (source.id)}
      {@const fault=failures[source.id] || safe(source.error)}
      <article class="source-row"><div class="source-details"><h3>{safe(source.name)}</h3>
        <div class="toolbar"><span class="badge">{source.enabled ? $t('Enabled','启用') : $t('Disabled','停用')}</span>
          {#if source.cached && source.lastSyncedAt}<span class="badge">{$t('Cached','缓存')}</span>{/if}
          <span class="subtle">{source.lastSyncedAt ? `${$t('Last sync','上次同步')}: ${formatDateTime(source.lastSyncedAt)}` : $t('Not synced','未同步')}</span></div>
        {#if fault}<div class="error-line" role="alert">{fault}</div>{/if}
        {#if busy === source.id}<small role="status">{$t('Working…','处理中…')}</small>{/if}
      </div>
      <div class="toolbar"><label class="toggle"><input type="checkbox" checked={source.enabled} aria-label={$t('Enable subscription','启用订阅')} disabled={!!busy || !!editing} on:change={(e) => toggle(source,e.currentTarget)}/>{$t('Enable','启用')}</label>
        <button class="secondary-button action" disabled={!!busy || !!editing} on:click={() => refresh(source)}><RefreshCw size={15}/>{$t('Refresh','更新')}</button>
        <button class="icon-button" disabled={!!busy || !!editing} aria-label={$t('Edit','编辑')} title={$t('Edit','编辑')} on:click={(e) => edit(source,e)}><Pencil size={15}/></button>
        <button class="icon-button" disabled={!!busy || !!editing} aria-label={$t('Delete','删除')} title={$t('Delete','删除')} on:click={() => remove(source)}><Trash2 size={15}/></button>
      </div></article>
    {:else}<div class="blank-state"><strong>{$t('No subscription sources','暂无订阅来源')}</strong><p>{$t('Add a source above, then refresh it to load its nodes.','在上方添加来源，然后更新以载入节点。')}</p></div>{/each}
  </section>
</div>
<style>
  .editor { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:12px; }
  .editor p,.editor .toolbar,.editor .error-line { grid-column:1/-1; margin:0; }
  .source-row { display:flex; align-items:center; justify-content:space-between; flex-wrap:wrap; gap:14px; padding:18px; }
  .source-row + .source-row { border-top:1px solid var(--border); }
  .source-details { min-width:0; overflow-wrap:anywhere; } h3 { font-size:15px; margin:0 0 8px; }
  .toggle { display:inline-flex; align-items:center; gap:4px; }
  @media(max-width:600px) { .editor { grid-template-columns:1fr; } }
</style>
