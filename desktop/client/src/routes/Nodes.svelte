<script>
  import { Plus, Search, RefreshCw, Upload, Download, Star } from 'lucide-svelte'
  import { t } from '../lib/i18n.js'
  import { downloadText } from '../lib/security.js'
  import ManualNodeDialog from '../lib/components/ManualNodeDialog.svelte'
  import NodeTable from '../lib/components/NodeTable.svelte'
  export let state
  export let runtime
  let query = ''
  let sort = 'latency'
  let editorOpen = false
  let editorNode = null
  let message = ''
  let source='all', favoritesOnly=false, busy=false
  $: nodes=state.subscription.nodes
  $: filtering=!!query.trim() || source !== 'all' || favoritesOnly
  $: filtered = nodes
    .filter((node) => `${node.name} ${node.subtitle || ''} ${node.sourceName || ''} ${node.address || ''}`.toLowerCase().includes(query.trim().toLowerCase()))
    .filter((node) => (source === 'all' || (node.sourceId || node.source) === source) && (!favoritesOnly || node.favorite))
    .sort((a,b) => sort === 'name' ? a.name.localeCompare(b.name) : (a.latencyMs ?? Infinity) - (b.latencyMs ?? Infinity))
  function resetFilters() { query=''; favoritesOnly=false; source='all' }

  function openEditor(node = null) { editorNode = node; editorOpen = true; message = '' }
  async function saveNode(node) { await runtime.saveManualNode(node); editorOpen=false; message=$t('Node saved','节点已保存') }
  async function deleteNode(node) {
    if (!window.confirm(`${$t('Delete node','删除节点')} ${node.name}?`)) return
    try { await runtime.deleteManualNode(node.id); message=$t('Node deleted','节点已删除') } catch (error) { message=String(error) }
  }
  function duplicate(node) { if(!node.config) { message=$t('This compact subscription node has no full profile to duplicate','该精简订阅节点尚无完整配置可复制'); return } openEditor({...node,id:null,name:`${node.name} copy`,source:'manual'}) }
  async function probe() { busy=true; try { await runtime.probeNodes(filtered.map((node) => node.id)) } catch(cause) { message=String(cause) } finally { busy=false } }
  async function importNodes(event) {
    const input=event.currentTarget, file=input.files[0]
    if(!file) return
    try {
      if(file.size > 2*1024*1024) throw new Error($t('File exceeds 2 MiB','文件超过 2 MiB'))
      const values=JSON.parse(await file.text())
      if(!Array.isArray(values)) throw new Error($t('Expected a node array','文件应为节点数组'))
      for(const node of values) await runtime.saveManualNode({...node,id:null})
      message=$t('Imported','导入完成')
    } catch(cause) { message=String(cause) } finally { input.value='' }
  }
  function exportNodes() { if(!confirm($t('Export includes private keys. Continue?','导出包含私钥，继续？'))) return; downloadText('openppp2-nodes.json',JSON.stringify(state.subscription.nodes.filter((node) => node.source === 'manual').map(({name,subtitle,config,options}) => ({name,subtitle,config,options})),null,2),'application/json') }
</script>

<div class="page">
  <div class="page-heading"><div><h1>{$t('Nodes','节点')}</h1><p class="page-lead">{$t('Find a server by name, source or address. Connecting switches the current session.','按名称、来源或地址查找服务器。连接其他节点会切换当前会话。')}</p></div><button class="primary-button action" on:click={() => openEditor()}><Plus size={15}/>{$t('Add node','添加节点')}</button></div>
  <section class="card">
    <div class="card-head"><h2>{$t('Node library','节点列表')}</h2><span class="subtle" role="status">{filtered.length} / {nodes.length} {$t('shown','个可见')}</span></div>
    <div class="toolbar-area">
      <label class="search"><Search size={14} /><input bind:value={query} placeholder={$t('Search nodes','搜索节点')} aria-label={$t('Search nodes','搜索节点')} /></label>
      <select class="select-input" bind:value={sort} aria-label={$t('Sort','排序')}><option value="latency">{$t('Latency','延迟')}</option><option value="name">{$t('Name','名称')}</option></select>
      <select class="select-input" bind:value={source} aria-label={$t('Source','来源')}><option value="all">{$t('All sources','全部来源')}</option><option value="manual">{$t('Local','本地')}</option>{#each state.subscription.sources || [] as item}<option value={item.id}>{item.name}</option>{/each}</select>
      <button class="secondary-button action" class:active={favoritesOnly} aria-pressed={favoritesOnly} on:click={() => favoritesOnly=!favoritesOnly}><Star size={15}/>{$t('Favorites','收藏')}</button>
      {#if filtering && filtered.length}<button class="inline-link" on:click={resetFilters}>{$t('Reset filters','重置筛选')}</button>{/if}
    </div>
    <div class="library-tools toolbar"><button class="secondary-button action" disabled={busy || !filtered.length} aria-label={$t('Probe TCP latency','测 TCP 延迟')} on:click={probe}><RefreshCw size={15}/>{busy ? $t('Probing…','测量中…') : $t('Probe TCP latency','测 TCP 延迟')}</button>
      <label class="secondary-button action file-button"><Upload size={15}/>{$t('Import nodes','导入节点')}<input type="file" accept=".json,application/json" on:change={importNodes} aria-label={$t('Import nodes','导入节点')}/></label>
      <button class="secondary-button action" disabled={!nodes.some((node) => node.source === 'manual')} on:click={exportNodes}><Download size={15}/>{$t('Export local nodes','导出本地节点')}</button>
    </div>
    {#if message}<div class="status-line" role="status">{message}</div>{/if}
    {#if filtered.length}
      <NodeTable nodes={filtered} currentNodeId={state.connection.currentNodeId} {runtime} showFavorite onEdit={openEditor} onDelete={deleteNode} onCopy={duplicate}/>
    {:else if nodes.length}
      <div class="blank-state"><strong>{$t('No matching nodes','没有匹配的节点')}</strong><p>{$t('Try another search or include more sources.','尝试其他关键词或扩大来源范围。')}</p><button class="secondary-button" on:click={resetFilters}>{$t('Reset filters','重置筛选')}</button></div>
    {:else}
      <div class="blank-state"><strong>{$t('No nodes yet','暂无节点')}</strong><p>{$t('Add a local node, import a JSON file, or refresh a subscription.','可添加本地节点、导入 JSON 文件，或更新订阅。')}</p><button class="secondary-button" on:click={() => runtime.navigate('subscription')}>{$t('Manage subscriptions','管理订阅')}</button></div>
    {/if}
  </section>
</div>

{#if editorOpen}<ManualNodeDialog node={editorNode} onClose={() => (editorOpen = false)} onSave={saveNode} />{/if}

<style>
  .toolbar-area,.library-tools { padding:12px 16px; display:flex; flex-wrap:wrap; gap:8px; border-bottom:1px solid var(--border); }
  .search { min-width: 180px; max-width: 420px; flex: 1; height: 34px; display: flex; align-items: center; gap: 8px; border: 1px solid var(--border-strong); border-radius: 7px; padding: 0 10px; background: var(--surface); color: var(--text-3); }
  .file-button { position:relative; cursor:pointer; } .file-button input { position:absolute; inset:0; width:100%; opacity:0; cursor:pointer; } .active { color:var(--accent); }
  .search input { min-width: 0; flex: 1; border: 0; outline: 0; background: transparent; color: var(--text); }
  .select-input { width: 112px; padding-top: 0; padding-bottom: 0; }
  .search:focus-within,.file-button:focus-within { outline:2px solid var(--accent); outline-offset:2px; }
  .status-line { padding:8px 16px; color:var(--text-2); overflow-wrap:anywhere; }
  @media(max-width:600px) { .search { min-width:100%; max-width:none; } .select-input { flex:1; } }
</style>
