<script>
  import { onDestroy, onMount } from 'svelte'
  import Sidebar from './lib/components/Sidebar.svelte'
  import Topbar from './lib/components/Topbar.svelte'
  import Connection from './routes/Connection.svelte'
  import Nodes from './routes/Nodes.svelte'
  import Subscription from './routes/Subscription.svelte'
  import Logs from './routes/Logs.svelte'
  import Settings from './routes/Settings.svelte'
  import Routing from './routes/Routing.svelte'
  import Dns from './routes/Dns.svelte'
  import Paths from './routes/Paths.svelte'
  import { language } from './lib/i18n.js'
  import { createRuntime } from './lib/runtime/index.js'
  import { connectionControls, runConnectionAction } from './lib/connectionControls.js'

  const runtime = createRuntime()
  let state
  let unsubscribe
  let startupError = ''
  let shortcutError = ''
  let initialized=runtime.kind === 'demo'
  let systemDark = window.matchMedia('(prefers-color-scheme: dark)').matches
  $: if (state) language.set(state.settings.language)
  $: theme = state?.settings.appearance || 'system'
  $: multipathConfigEnabled = (() => { try { return JSON.parse(state?.config || '{}')?.client?.multipath?.enabled === true } catch { return false } })()
  $: multipathVisible = multipathConfigEnabled || (['connected', 'reconnecting'].includes(state?.connection?.status) && state.connection.statsAvailable && state.stats?.multipath?.enabled === true)
  $: if (state?.route === 'paths' && !multipathVisible) runtime.navigate('connection')
  $: if(state && initialized) { localStorage.setItem('openppp2-appearance',theme); document.documentElement.lang=state.settings.language === 'English' ? 'en' : 'zh-CN' }
  $: if(initialized) document.documentElement.dataset.theme = theme === 'light' || theme === '浅色' ? 'light' : theme === 'dark' || theme === '深色' ? 'dark' : systemDark ? 'dark' : 'light'

  async function handleKeydown(event) {
    if (!(event.ctrlKey || event.metaKey)) return
    const key = event.key.toLowerCase()
    if (key === 'k') {
      event.preventDefault()
      runtime.navigate('nodes')
    }
    if (key === 'd') {
      if(event.defaultPrevented || event.isComposing || event.shiftKey || event.altKey || !initialized || !state) return
      if(event.target?.closest?.('input,textarea,select,[contenteditable]:not([contenteditable="false"])') || document.querySelector('dialog[open],[aria-modal="true"],[role="dialog"]')) return
      event.preventDefault()
      if(event.repeat) return
      if(state.route === 'connection') { document.getElementById('connection-power')?.click(); return }
      // Away from Connection, keep this a connect/disconnect shortcut. Recovery
      // and cancellation have dedicated, labelled actions on Connection.
      if(!['connected','disconnected','error'].includes(state.connection.status)) return
      shortcutError=''
      try { await runConnectionAction(runtime,connectionControls(state,runtime.kind)) } catch(cause) { shortcutError=String(cause) }
    }
  }

  onMount(() => {
    unsubscribe = runtime.subscribe((next) => (state = next))
    window.addEventListener('keydown', handleKeydown)
    const media = window.matchMedia('(prefers-color-scheme: dark)')
    const update = (event) => { systemDark = event.matches }
    media.addEventListener('change', update)
    runtime.ready?.then(() => initialized=true).catch((error) => { startupError = String(error) })
    return () => media.removeEventListener('change', update)
  })

  onDestroy(() => {
    unsubscribe?.()
    window.removeEventListener('keydown', handleKeydown)
  })
</script>

{#if state}
  <div class="shell">
    <Sidebar current={state.route} navigate={(route) => runtime.navigate(route)} version={state.kernel?.version} {multipathVisible} />
    <main>
      <Topbar {state} {runtime} />
      <div class="content">
        {#if startupError}<div role="alert" class="notice">{startupError}</div>{/if}
        {#if shortcutError}<div role="alert" class="error-line">{shortcutError}</div>{/if}
        {#if state.route === 'connection'}
          <Connection {state} {runtime} />
        {:else if state.route === 'nodes'}
          <Nodes {state} {runtime} />
        {:else if state.route === 'subscription'}
          <Subscription {state} {runtime} />
        {:else if state.route === 'logs'}
          <Logs {state} {runtime} />
        {:else if state.route === 'routing'}
          <Routing {state} {runtime} />
        {:else if state.route === 'dns'}
          <Dns {state} {runtime} />
        {:else if state.route === 'paths' && multipathVisible}
          <Paths {state} />
        {:else if state.route === 'settings'}
          <Settings {state} {runtime} />
        {/if}
      </div>
    </main>
  </div>
{/if}

<style>
  .shell { min-height: 100vh; display: grid; grid-template-columns: 196px minmax(0, 1fr); }
  main { min-width: 0; padding: 0 24px 18px; }
  .content { max-width: 1100px; margin: 0; }
  @media (max-width: 900px) { .shell { grid-template-columns: 58px minmax(0,1fr); } main { padding: 0 16px 18px; } }
  @media (max-width: 560px) { .shell { grid-template-columns:58px minmax(0,1fr); } main { padding:0 10px 24px; } }
</style>
