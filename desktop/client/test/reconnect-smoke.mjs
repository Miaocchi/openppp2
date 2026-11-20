import assert from 'node:assert/strict'

// Exercise the real routes and Tauri JS runtime with deterministic native replies.
// No external executable, host network setting or administrator prompt is used.
export async function checkPendingReconnect(browser, url) {
  const page = await browser.newPage()
  try {
    await page.addInitScript(() => {
      let saves = 0
      let savedRules = 'default proxy\n'
      window.disconnectAttempts = 0
      window.__TAURI__ = {
        event: { listen: async () => () => {} },
        core: { async invoke(command, args) {
          switch (command) {
            case 'client_bootstrap': return {
              currentNodeId: 'test-node', connection: { status: 'connected', sessionId: 17, pid: 123 },
              subscription: { sources: [], nodes: [{ id: 'test-node', name: 'Test server' }] },
              settings: { language: 'English', connectionMode: 'proxy' }, policyEnabled: true,
            }
            case 'client_probe_latency': return {}
            case 'policy_load': return { enabled: true, kernelSupported: true, policy: { version: 2 }, rules: savedRules }
            case 'policy_save': {
              const exitCode = ++saves === 1 ? 0 : 1
              if (exitCode === 0) savedRules = args.rules
              return { result: { exitCode, report: { diagnostics: [] } } }
            }
            case 'client_disconnect': window.disconnectAttempts++; throw new Error('Test disconnect refused')
            default: throw new Error(`Unexpected test command: ${command} ${JSON.stringify(args)}`)
          }
        } },
      }
    })
    await page.goto(url)
    const button = (name) => page.getByRole('button', { name, exact: true })
    await button('Routing').click()
    await page.getByRole('tab', { name: 'Text', exact: true }).click()
    const rules = page.getByRole('textbox', { name: 'Rules file' })
    await rules.fill('default direct\n')
    await rules.press('Control+d')
    await button('Save').click()
    await button('Reconnect now').waitFor()
    await rules.fill('invalid draft\n')
    await button('Save and reconnect').click()
    await page.getByRole('alert').filter({ hasText: 'kernel rejected' }).waitFor()
    assert.equal(await page.evaluate(() => window.disconnectAttempts), 0, 'A previous saved change must not authorize reconnect after rejection')
    await button('Connection').click()
    await button('Reconnect now').click()
    await page.getByRole('alert').filter({ hasText: 'Test disconnect refused' }).waitFor()
    assert(await button('Reconnect now').isEnabled(), 'A failed switch must retain the action for this session')
    await button('Routing').click()
    await button('Reconnect now').waitFor()
    assert.equal(await page.evaluate(() => window.disconnectAttempts), 1)
  } finally { await page.close() }
}
