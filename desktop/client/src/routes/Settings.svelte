<script>
  export let state
  export let runtime
  const toggleRows = [
    { key: 'autostart', label: '开机启动', description: '登录系统后自动启动 Client 管理器' },
    { key: 'closeToTray', label: '关闭到托盘', description: '关闭窗口时保持程序在后台运行' },
    { key: 'disconnectOnExit', label: '退出时断开', description: '从托盘退出程序时结束当前 ppp 连接' },
  ]
</script>

<div class="page">
  <section class="panel">
    <div class="panel-head"><h1 class="panel-title">设置</h1></div>
    <div class="setting-list">
      <label class="setting-row path-row"><span><b>ppp.exe</b><small>Windows 外部程序；留空时使用 Client 同目录下的 ppp.exe</small></span><input class="text-input mono" value={state.settings.pppPath} on:change={(event) => runtime.updateSetting('pppPath', event.currentTarget.value.trim())} placeholder="ppp.exe" /></label>
      <div class="setting-row"><span><b>连接方式</b><small>虚拟网卡会改系统路由；本地代理只监听 HTTP/SOCKS</small></span><select class="select-input" value={state.settings.connectionMode} on:change={(event) => runtime.updateSetting('connectionMode', event.currentTarget.value)}><option value="client">虚拟网卡</option><option value="proxy">本地代理</option></select></div>
      {#each toggleRows as row}
        <label class="setting-row"><span><b>{row.label}</b><small>{row.description}</small></span><input type="checkbox" checked={state.settings[row.key]} on:change={(event) => runtime.updateSetting(row.key, event.currentTarget.checked)} /><i></i></label>
      {/each}
      <div class="setting-row"><span><b>语言</b><small>Client 管理器界面语言</small></span><select class="select-input" value={state.settings.language} on:change={(event) => runtime.updateSetting('language', event.currentTarget.value)}><option>简体中文</option><option>English</option></select></div>
      <div class="setting-row"><span><b>外观</b><small>当前设计仅提供深色主题</small></span><select class="select-input" value={state.settings.appearance} disabled><option>深色</option></select></div>
    </div>
  </section>
</div>

<style>
  .setting-list { padding: 0 18px; }
  .setting-row { min-height: 64px; display: flex; align-items: center; justify-content: space-between; gap: 20px; border-bottom: 1px solid var(--border); position: relative; }
  .setting-row:last-child { border-bottom: 0; }
  .setting-row span { display: grid; gap: 4px; }.setting-row b { font-size: 12px; }.setting-row small { color: var(--text-3); }
  .setting-row input[type="checkbox"] { position: absolute; opacity: 0; pointer-events: none; }
  .setting-row i { width: 34px; height: 19px; border-radius: 10px; background: #2c323a; position: relative; cursor: pointer; transition: background-color 150ms; }
  .setting-row i::after { content: ''; position: absolute; top: 3px; left: 3px; width: 13px; height: 13px; border-radius: 50%; background: #a3abb4; transition: transform 150ms; }
  .setting-row input:checked + i { background: #edf0f4; }.setting-row input:checked + i::after { background: #0b0d10; transform: translateX(15px); }
  .setting-row select { width: 132px; }
  .path-row input { width: min(420px, 52%); }
</style>
