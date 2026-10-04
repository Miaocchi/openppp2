import assert from 'node:assert/strict'
import { resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
const {chromium}=await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href)
const browser=await chromium.connectOverCDP('http://127.0.0.1:9223')
try {
  const page=browser.contexts()[0].pages().find((page) => page.url().includes('tauri')) || browser.contexts()[0].pages()[0]
  await page.waitForFunction(() => !!window.__TAURI__)
  const invoke=(command,args={}) => page.evaluate(({command,args}) => window.__TAURI__.core.invoke(command,args),{command,args})
  let bootstrap=await invoke('client_bootstrap')
  for (const node of bootstrap.subscription.nodes) await invoke('client_delete_manual_node',{nodeId:node.id})
  await invoke('client_update_setting',{key:'pppPath',value:resolve('x64/Release/ppp.exe')})
  const info=await invoke('client_kernel_info')
  assert.match(info.version,/\d+\.\d+\.\d+/)
  await invoke('client_update_setting',{key:'connectionMode',value:'proxy'})
  await invoke('client_update_setting',{key:'autoSystemProxy',value:false})
  await invoke('client_update_network',{overrides:{dns:{servers:{domestic:'doh.pub',foreign:'cloudflare'}}}})
  const local=await invoke('client_upsert_manual_node',{node:{id:null,name:'Native QA',subtitle:'',config:{key:{'protocol-key':'qa-only','transport-key':'qa-only'},client:{server:'ppp://127.0.0.1:1/','http-proxy':{bind:'127.0.0.1',port:18081},'socks-proxy':{bind:'127.0.0.1',port:18082}}},options:{}}})
  const id=local.nodes[0].id
  const preview=await invoke('client_preview',{nodeId:id})
  assert.equal(preview.config.key['protocol-key'],'[redacted]')
  assert.equal(preview.config.dns.servers.domestic,'doh.pub')
  assert(preview.args.includes('--mode=proxy'))
  let spawnResult='passed'
  try {
    const first=await invoke('client_connect',{nodeId:id})
    assert(first.pid > 0 && first.sessionId > 0)
    await page.waitForTimeout(1500)
    await invoke('client_disconnect')
    bootstrap=await invoke('client_bootstrap')
    assert.equal(bootstrap.connection.pid,null)
    assert.equal(bootstrap.connection.status,'disconnected')
    const second=await invoke('client_connect',{nodeId:id})
    assert(second.sessionId > first.sessionId)
  } catch (error) {
    if (!String(error).includes('740')) throw error
    spawnResult='blocked: executable requires elevation (740)'
    assert.equal((await invoke('client_bootstrap')).connection.pid,null)
  } finally { await invoke('client_disconnect') }
  await invoke('client_delete_manual_node',{nodeId:id})
  assert.equal((await invoke('client_bootstrap')).subscription.nodes.length,0)
  console.log(JSON.stringify({result:'Native Tauri command checks passed',spawnResult,version:info.version,statsSupported:info.statsSupported,checks:['bootstrap','kernel inspection','network save','manual node','redacted preview','disconnect','delete']}))
} finally { await browser.close() }
