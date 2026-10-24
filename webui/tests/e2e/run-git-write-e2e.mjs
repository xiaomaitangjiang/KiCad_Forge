// Exercises the migrated Git panel against a fresh synthetic repository only.
// KFORGE_GIT_WRITE_E2E=1 KFORGE_BUILD_ROOT=<empty-data package> node <this file>
import { chmodSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { check, requireOptIn, runAppFixture } from '../support/plugin-e2e-harness.mjs'

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

async function main() {
  if (!requireOptIn('KFORGE_GIT_WRITE_E2E')) return
  await runAppFixture('git-write', async ({ root, launch, stop, json, waitFor, results }) => {
    const libraryDir = path.join(root, 'git-fixture')
    mkdirSync(libraryDir)
    const library = path.join(libraryDir, 'GitE2E.kicad_sym')
    const original = '(kicad_symbol_lib (version 20231120))\n'
    writeFileSync(library, original)
    // Never inherit user hooks, signing settings, credentials or global config.
    // The isolated hook only delays one commit so the E2E can switch pages while
    // the write task is still running; it is never inherited from the machine.
    const hooksDir = path.join(root, 'git-hooks')
    mkdirSync(hooksDir)
    const hook = path.join(hooksDir, 'pre-commit')
    writeFileSync(hook, [
      '#!/bin/sh',
      'if [ -f ".git/kf-slow-once" ]; then',
      '  rm -f ".git/kf-slow-once"',
      '  sleep 6',
      'fi',
      'if [ -f ".git/kf-timeout-once" ]; then',
      '  rm -f ".git/kf-timeout-once"',
      '  sleep 31',
      'fi',
      'exit 0',
      '',
    ].join('\n'))
    try { chmodSync(hook, 0o755) } catch { /* Windows Git for Windows runs via sh */ }
    const gitConfig = path.join(root, 'git-fixture.config')
    writeFileSync(gitConfig, '[commit]\n\tgpgsign = false\n[core]\n\thooksPath = ' + hooksDir.replace(/\\/g, '/') + '\n')
    // M2: delay only Git read actions, and only while the flag file exists. The
    // injected sitecustomize is the real plugin process, so cancellation exercises
    // the actual backend read rather than a mocked frontend promise.
    const pyInject = path.join(root, 'py-inject')
    mkdirSync(pyInject)
    writeFileSync(path.join(pyInject, 'sitecustomize.py'), [
      'import os, sys, time',
      'flag = os.environ.get("KF_E2E_SLOW_READ_FILE")',
      'read = len(sys.argv) > 1 and sys.argv[1] in ("history", "commit_detail", "commit_file")',
      'if read and flag and os.path.exists(flag):',
      '    time.sleep(8)',
      '',
    ].join('\n'))
    const slowReadFlag = path.join(root, 'slow-read.flag')
    const session = await launch({ env: {
      GIT_AUTHOR_NAME: 'KF E2E', GIT_AUTHOR_EMAIL: 'e2e@example.invalid',
      GIT_COMMITTER_NAME: 'KF E2E', GIT_COMMITTER_EMAIL: 'e2e@example.invalid',
      GIT_CONFIG_NOSYSTEM: '1', GIT_CONFIG_GLOBAL: gitConfig,
      GIT_TERMINAL_PROMPT: '0', GIT_CEILING_DIRECTORIES: root,
      PYTHONPATH: pyInject, KF_E2E_SLOW_READ_FILE: slowReadFlag,
    } })
    const { cdp, origin } = session
    const records = []
    try {
      const added = await json(`${origin}/api/component-libraries`, {
        method: 'POST', headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          action: 'add', name: 'Git E2E Component Library',
          symbol_path: libraryDir, footprint_path: '',
        }),
      })
      check(added.ok === true && added.library?.id, `component library registration failed: ${JSON.stringify(added)}`)
      await cdp.send('Page.reload')
      await waitFor(() => cdp.eval('Boolean([...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "Git"))'), 20000, 'Git activity')
      await cdp.eval('[...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "Git")?.click()')
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-panel] [data-testid=git-target]"))'), 20000, 'Git sidebar panel')
      await waitFor(() => cdp.eval(`(() => {
        const select = document.querySelector('[data-testid=git-target]')
        const option = [...(select?.options || [])].find((item) => item.value === ${JSON.stringify(libraryDir)})
        if (!option) return false
        const setter = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set
        setter.call(select, option.value)
        select.dispatchEvent(new Event('change', { bubbles: true }))
        return select.value === option.value
      })()`), 10000, 'synthetic Git component library option')

      const tasks = async () => {
        const response = await json(`${origin}/api/plugins/com.kicad_forge.git_version/tasks`)
        check(response.ok === true && Array.isArray(response.tasks), `bad task list: ${JSON.stringify(response)}`)
        return response.tasks
      }
      const click = (text) => cdp.eval(`(() => {
        const button = [...document.querySelectorAll('.gf-git-panel button')]
          .find((item) => item.textContent.trim() === ${JSON.stringify(text)})
        if (!button || button.disabled) return false
        button.click()
        return true
      })()`)
      async function run(action, text) {
        await waitFor(() => cdp.eval('[...document.querySelectorAll(".gf-git-actions button")].every((button) => !button.disabled)'), 15000, `${action} idle`)
        const before = new Set((await tasks()).map((task) => task.task_id))
        check(await click(text), `${action}: button missing or disabled`)
        const record = await waitFor(async () => {
          const task = (await tasks()).find((item) => !before.has(item.task_id) && item.action === action)
          return task && ['succeeded', 'failed', 'cancelled'].includes(task.state) ? task : null
        }, 45000, `${action} task terminal`)
        records.push(record)
        writeFileSync(path.join(results, 'git-write.tasks.json'), `${JSON.stringify(records, null, 2)}\n`)
        check(record.state === 'succeeded' && record.result?.ok === true && record.uncertain !== true,
          `${action}: expected successful action, got ${JSON.stringify(record)}`)
        check(record.cancelable === !['init', 'commit'].includes(action), `${action}: incorrect cancelable flag`)
        await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-task-result]"))'), 10000, `${action} result display`)
        console.log(`${action}: succeeded`)
        return record
      }

      const initialized = await run('init', '初始化仓库')
      check(initialized.result.initialized === true && initialized.result.committed === true,
        `init did not create its initial commit: ${JSON.stringify(initialized)}`)
      const status = await run('status', '状态')
      check(status.result.initialized === true, `status did not find fixture repository: ${JSON.stringify(status)}`)
      check(path.resolve(status.result.root) === path.resolve(libraryDir), `Git escaped the synthetic repository: ${JSON.stringify(status)}`)

      // P3: the status bar opens the host task overview; it observes Git's own
      // task handles and never creates a second task or polling loop.
      await cdp.eval('document.querySelector("[data-testid=status-tasks]")?.click()')
      await waitFor(() => cdp.eval('document.querySelectorAll("[data-testid=bottom-task-list] .bottom-panel-task").length >= 2'), 10000, 'bottom task overview')
      check(await cdp.eval('document.querySelector("[data-testid=bottom-task-list]").innerText.includes("init") && document.querySelector("[data-testid=bottom-task-list]").innerText.includes("status")'), 'task overview did not show both Git tasks')
      const bottomTabs = await cdp.eval('[...document.querySelectorAll(".bottom-panel-tab")].map((tab) => tab.dataset.view)')
      check(Array.isArray(bottomTabs) && bottomTabs.length === 2 && bottomTabs.includes('tasks') && bottomTabs.includes('problems'),
        'bottom panel must only expose tasks and problems: ' + JSON.stringify(bottomTabs))
      check(await cdp.eval('document.querySelectorAll("[data-testid=bottom-task-list] .bottom-panel-task-copy").length >= 2'),
        'task rows must expose per-task diagnostics copy')
      check(await cdp.eval('document.querySelector("[data-testid=bottom-task-output]") === null'),
        'legacy output view must not exist')
      await cdp.eval('document.querySelector(".bottom-panel-close")?.click()')
      const initialHistory = await run('history', '历史')
      check(initialHistory.result.commits?.some((commit) => commit.subject.includes('KF: initial commit')) === true,
        'initial commit is absent from history: ' + JSON.stringify(initialHistory.result))

      // A commit needs an actual edit; unchanged content must not pass.
      writeFileSync(library, `${original.trimEnd()}\n; changed by isolated CI fixture\n`)
      const diff = await run('diff', '差异')
      check(diff.result.output.includes('GitE2E.kicad_sym'), `diff did not see the synthetic edit: ${JSON.stringify(diff)}`)
      await cdp.eval(`(() => {
        const input = document.querySelector('[data-testid=git-message]')
        const proto = input.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype
        const setter = Object.getOwnPropertyDescriptor(proto, 'value').set
        setter.call(input, 'KF e2e commit')
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })()`)
      // P3 timing: a slow isolated pre-commit hook keeps the write task running
      // while the workbench switches center pages. The same task must survive the
      // switch, finish once, and never be duplicated or restarted.
      writeFileSync(path.join(libraryDir, '.git', 'kf-slow-once'), '1\n')
      await waitFor(() => cdp.eval('[...document.querySelectorAll(".gf-git-actions button")].every((button) => !button.disabled)'), 15000, 'commit idle')
      const beforeCommit = new Set((await tasks()).map((task) => task.task_id))
      check(await click('提交'), 'commit: button missing or disabled')
      const runningCommit = await waitFor(async () => {
        const task = (await tasks()).find((item) => !beforeCommit.has(item.task_id) && item.action === 'commit')
        return task && ['queued', 'running'].includes(task.state) ? task : null
      }, 15000, 'commit running')
      const switchCenterTab = (label) => cdp.eval(`(() => {
        const tab = [...document.querySelectorAll('.center-tab-label')]
          .find((item) => item.textContent.trim() === ${JSON.stringify(label)})
        if (!tab) return false
        tab.click()
        return true
      })()`)
      check(await switchCenterTab('全部元件'), 'commit: home center tab missing')
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".center-stack-layer:not([hidden]) .toolbar"))'), 10000, 'home center page while commit runs')
      // H1: closing the center home view must not cancel or duplicate a running
      // Git write task; reopening from the sidebar must show the same task.
      await cdp.eval('document.querySelector(".center-tab.active .center-tab-close")?.click()')
      await waitFor(() => cdp.eval('document.querySelectorAll(".center-tab-label").length === 0'), 5000, 'home closes while commit runs')
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=center-empty-open-home]"))'), 5000, 'empty center while commit runs')
      const whileHomeClosed = (await tasks()).find((item) => item.task_id === runningCommit.task_id)
      check(whileHomeClosed && ['queued', 'running', 'succeeded'].includes(whileHomeClosed.state),
        'commit was cancelled while the home view was closed: ' + JSON.stringify(whileHomeClosed))
      // The Git view replaces the workspace sidebar, so reopen the home view from
      // the workspace activity and then return to Git.
      await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].find(node => node.title === '工作空间')?.click()`)
      await waitFor(() => cdp.eval('!document.querySelector("[data-testid=git-panel]")'), 10000, 'workspace activity after home close')
      await cdp.eval(`[...document.querySelectorAll('.sidebar-navigation .nav-item')].find(node => node.textContent.includes('全部元件'))?.click()`)
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".center-stack-layer:not([hidden]) .toolbar"))'), 10000, 'home reopened while commit runs')
      await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].find(node => node.title === 'Git')?.click()`)
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".gf-git-panel [data-testid=git-message]"))'), 10000, 'Git activity restored while commit runs')
      await sleep(400)
      const duringCommit = (await tasks()).find((item) => item.task_id === runningCommit.task_id)
      check(duringCommit && ['queued', 'running'].includes(duringCommit.state),
        'commit was cancelled or duplicated by switching center pages: ' + JSON.stringify(duringCommit))
      // The Git UI lives only in the primary sidebar (WP-001/002); switching
      // away and back remounts the panel around the same model and task.
      const switchActivity = (label) => cdp.eval(`(() => {
        const item = [...document.querySelectorAll('.activitybar .nav-item')]
          .find((node) => node.title === ${JSON.stringify(label)})
        if (!item) return false
        item.click()
        return true
      })()`)
      check(await switchActivity('工作空间'), 'commit: workspace activity missing')
      await waitFor(() => cdp.eval('!document.querySelector("[data-testid=git-panel]")'), 10000, 'Git panel hidden after switching activity')
      check(await switchActivity('Git'), 'commit: Git activity missing')
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".gf-git-panel [data-testid=git-message]"))'), 10000, 'Git sidebar panel after switching activity')
      const terminalCommit = await waitFor(async () => {
        const task = (await tasks()).find((item) => item.task_id === runningCommit.task_id)
        return task && ['succeeded', 'failed', 'cancelled'].includes(task.state) ? task : null
      }, 45000, 'commit terminal after page switch')
      records.push(terminalCommit)
      writeFileSync(path.join(results, 'git-write.tasks.json'), `${JSON.stringify(records, null, 2)}\n`)
      check(terminalCommit.state === 'succeeded' && terminalCommit.result?.ok === true && terminalCommit.uncertain !== true,
        `commit: expected successful action across page switch, got ${JSON.stringify(terminalCommit)}`)
      check(terminalCommit.task_id === runningCommit.task_id, 'commit task id changed across page switch')
      const commitTasks = (await tasks()).filter((item) => item.action === 'commit')
      check(commitTasks.filter((item) => item.task_id === runningCommit.task_id).length === 1,
        'commit task was duplicated across page switch')
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-task-result]"))'), 10000, 'commit result after page switch')
      const finalHistory = await run('history', '历史')
      check(finalHistory.result.commits?.some((commit) => commit.subject.includes('KF e2e commit')) === true,
        'new commit is absent from history: ' + JSON.stringify(finalHistory.result))
      check(readFileSync(library, 'utf8').includes('changed by isolated CI fixture'), 'fixture content unexpectedly changed')

      // P3 recovery: reload while a backend task is running. The new frontend
      // session must adopt the same task into the host overview and never create
      // a second one or lose its terminal result.
      await cdp.eval('document.querySelector("[data-testid=status-tasks]")?.click()')
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=bottom-task-list]"))'), 10000, 'bottom tasks before reload')
      await sleep(300)
      writeFileSync(library, `${original.trimEnd()}\n; changed by isolated CI fixture\n; reload recovery edit\n`)
      await cdp.eval(`(() => {
        const input = document.querySelector('[data-testid=git-message]')
        const proto = input.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype
        const setter = Object.getOwnPropertyDescriptor(proto, 'value').set
        setter.call(input, 'KF e2e reload commit')
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })()`)
      await waitFor(() => cdp.eval('[...document.querySelectorAll(".gf-git-actions button")].every((button) => !button.disabled)'), 15000, 'reload commit idle')
      const beforeReload = new Set((await tasks()).map((task) => task.task_id))
      writeFileSync(path.join(libraryDir, '.git', 'kf-slow-once'), '1\n')
      check(await click('提交'), 'reload commit: button missing or disabled')
      const reloadCommit = await waitFor(async () => {
        const task = (await tasks()).find((item) => !beforeReload.has(item.task_id) && item.action === 'commit')
        return task && ['queued', 'running'].includes(task.state) ? task : null
      }, 15000, 'reload commit running')
      const reloadTaskId = reloadCommit.task_id
      await cdp.send('Page.reload')
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'workbench after reload')
      if (!(await cdp.eval('Boolean(document.querySelector(".bottom-panel"))'))) {
        await cdp.eval('document.querySelector("[data-testid=status-tasks]")?.click()')
      }
      await waitFor(() => cdp.eval(`(() => {
        const node = document.querySelector('[data-testid=bottom-task-list] [title="${reloadTaskId}"]')
        return node?.parentElement?.dataset.state || null
      })()`), 30000, 'recovered task in bottom overview')
      const recoveredTerminal = await waitFor(async () => {
        const task = (await tasks()).find((item) => item.task_id === reloadTaskId)
        if (!task || !['succeeded', 'failed', 'cancelled'].includes(task.state)) return null
        const uiState = await cdp.eval(`document.querySelector('[data-testid=bottom-task-list] [title="${reloadTaskId}"]')?.parentElement?.dataset.state || null`)
        return uiState === task.state ? task : null
      }, 45000, 'recovered commit terminal')
      check(recoveredTerminal.state === 'succeeded' && recoveredTerminal.result?.ok === true && recoveredTerminal.uncertain !== true,
        'recovered task did not preserve its result: ' + JSON.stringify(recoveredTerminal))
      check((await tasks()).filter((item) => item.task_id === reloadTaskId).length === 1,
        'reload duplicated the backend task in the overview')
      records.push(recoveredTerminal)
      writeFileSync(path.join(results, 'git-write.tasks.json'), `${JSON.stringify(records, null, 2)}\n`)
      console.log('reload commit: succeeded')
      if (!(await cdp.eval('Boolean(document.querySelector(".gf-git-panel [data-testid=git-message]"))'))) {
        await cdp.eval('[...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "Git")?.click()')
        await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-panel] [data-testid=git-target]"))'), 20000, 'Git sidebar panel after reload')
      }
      const recoveredHistory = await run('history', '历史')
      check(recoveredHistory.result.commits?.some((commit) => commit.subject.includes('KF e2e reload commit')) === true,
        'recovered commit is absent from history: ' + JSON.stringify(recoveredHistory.result))
      const clean = await run('status', '状态')
      check(clean.result.changes === 'clean', `working tree not clean after commit: ${JSON.stringify(clean)}`)

      // M2: a real read cancellation keeps the previous result, reports the
      // cancelled phase, and retries in place. The injected sitecustomize delays
      // the actual plugin history read, so this is not a mocked frontend promise.
      const historyForCancel = await run('history', '历史')
      const expectedRows = historyForCancel.result.commits?.length || 0
      check(expectedRows > 0, 'history has no rows for the cancellation check')
      await waitFor(() => cdp.eval(`document.querySelectorAll(".gf-git-history-row").length === ${expectedRows}`), 10000, 'history rows before cancellation')
      const readTasksBefore = new Set((await tasks()).map((task) => task.task_id))
      await waitFor(() => cdp.eval('(() => { const button = document.querySelector("[data-testid=git-history-refresh]"); return Boolean(button) && !button.disabled })()'), 15000, 'history refresh idle')
      writeFileSync(slowReadFlag, '1\n')
      check(await cdp.eval('(() => { const button = document.querySelector("[data-testid=git-history-refresh]"); if (!button || button.disabled) return false; button.click(); return true })()'), 'history refresh button missing or disabled')
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-history-cancel]"))'), 5000, 'history cancel affordance')
      await waitFor(() => cdp.eval('document.querySelector(".gf-git-history-caption")?.innerText.includes("刷新中") === true'), 5000, 'history loading caption')
      const runningRead = await waitFor(async () => {
        const task = (await tasks()).find((item) => !readTasksBefore.has(item.task_id) && item.action === 'history')
        return task && ['queued', 'running'].includes(task.state) ? task : null
      }, 10000, 'history read task running')
      check(await cdp.eval('(() => { const button = document.querySelector("[data-testid=git-history-cancel]"); if (!button) return false; button.click(); return true })()'), 'history cancel button missing')
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-history-cancelled]"))'), 15000, 'history cancelled bar')
      const cancelledRead = await waitFor(async () => {
        const task = (await tasks()).find((item) => item.task_id === runningRead.task_id)
        return task && task.state === 'cancelled' ? task : null
      }, 15000, 'history read task cancelled')
      const m2Cancelled = await cdp.eval('(() => ({ rows: document.querySelectorAll(".gf-git-history-row").length, text: document.querySelector("[data-testid=git-history-cancelled]").innerText, caption: document.querySelector(".gf-git-history-caption")?.innerText || "", reading: document.body.innerText.includes("正在读取历史") }))()')
      check(m2Cancelled.rows === expectedRows, `history cancellation lost the previous rows: ${JSON.stringify(m2Cancelled)}`)
      check(m2Cancelled.reading === false, `history cancellation left a fake loading state: ${JSON.stringify(m2Cancelled)}`)
      check(m2Cancelled.text.includes('已取消') && m2Cancelled.text.includes('上次结果'), `history cancellation text is missing: ${JSON.stringify(m2Cancelled)}`)
      const m2Shot = await cdp.send('Page.captureScreenshot', { format: 'png' })
      writeFileSync(path.join(results, 'git-write-m2-cancelled.png'), Buffer.from(m2Shot.data, 'base64'))
      rmSync(slowReadFlag, { force: true })
      check(await cdp.eval('(() => { const button = document.querySelector("[data-testid=git-history-cancelled] .gf-git-retry"); if (!button) return false; button.click(); return true })()'), 'cancelled history has no in-place retry')
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-history-cancelled]")) === false'), 20000, 'history retry clears cancellation')
      await waitFor(() => cdp.eval(`document.querySelectorAll(".gf-git-history-row").length === ${expectedRows} && document.querySelector(".gf-git-history-caption")?.innerText.includes("已全部加载")`), 20000, 'history retry completes with rows')
      const m2Recovered = await cdp.eval('(() => ({ rows: document.querySelectorAll(".gf-git-history-row").length, caption: document.querySelector(".gf-git-history-caption")?.innerText || "", reading: document.body.innerText.includes("正在读取历史") }))()')
      check(m2Recovered.reading === false, `history retry left a fake loading state: ${JSON.stringify(m2Recovered)}`)
      writeFileSync(path.join(results, 'git-write.m2.json'), `${JSON.stringify({ expectedRows, cancelled: m2Cancelled, cancelledTask: cancelledRead, recovered: m2Recovered }, null, 2)}\n`)
      console.log('history cancellation: passed')

      // M3: the Git plugin's own 30s git timeout becomes a failed, uncertain
      // task. The UI must show a separate "result to verify" state, keep the
      // original failed state, and only offer read-only status/history checks.
      writeFileSync(library, `${original.trimEnd()}\n; changed by isolated CI fixture\n; reload recovery edit\n; timeout edit\n`)
      await cdp.eval(`(() => {
        const input = document.querySelector('[data-testid=git-message]')
        const proto = input.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype
        const setter = Object.getOwnPropertyDescriptor(proto, 'value').set
        setter.call(input, 'KF e2e timeout commit')
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })()`)
      await waitFor(() => cdp.eval('[...document.querySelectorAll(".gf-git-actions button")].every((button) => !button.disabled)'), 15000, 'timeout commit idle')
      const beforeTimeout = new Set((await tasks()).map((task) => task.task_id))
      const commitCountBeforeTimeout = (await tasks()).filter((item) => item.action === 'commit').length
      const initCountBeforeTimeout = (await tasks()).filter((item) => item.action === 'init').length
      writeFileSync(path.join(libraryDir, '.git', 'kf-timeout-once'), '1\n')
      check(await click('提交'), 'timeout commit: button missing or disabled')
      const timeoutCommit = await waitFor(async () => {
        const task = (await tasks()).find((item) => !beforeTimeout.has(item.task_id) && item.action === 'commit')
        return task && ['succeeded', 'failed', 'cancelled'].includes(task.state) ? task : null
      }, 70000, 'timeout commit terminal')
      records.push(timeoutCommit)
      writeFileSync(path.join(results, 'git-write.tasks.json'), `${JSON.stringify(records, null, 2)}\n`)
      check(timeoutCommit.state === 'failed', `timeout commit must fail: ${JSON.stringify(timeoutCommit)}`)
      check(timeoutCommit.uncertain === true, `timeout commit must be marked uncertain: ${JSON.stringify(timeoutCommit)}`)
      check(String(timeoutCommit.error || '').toLowerCase().includes('timed out'), `timeout commit error lacks the timeout cause: ${JSON.stringify(timeoutCommit)}`)
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-task-uncertain]"))'), 15000, 'Git uncertain result panel')
      const uncertainUi = await cdp.eval('(() => ({ text: document.querySelector("[data-testid=git-task-uncertain]").innerText, state: document.querySelector("[data-testid=git-task-uncertain-state]")?.innerText || "", verify: [...document.querySelectorAll("[data-testid=git-task-verify-status], [data-testid=git-task-verify-history]")].map((node) => node.innerText) }))()')
      check(uncertainUi.text.includes('待核对'), `Git uncertain panel lacks its label: ${JSON.stringify(uncertainUi)}`)
      check(uncertainUi.text.includes('failed') || uncertainUi.state.includes('failed'), `Git uncertain panel lost the failed state: ${JSON.stringify(uncertainUi)}`)
      check(uncertainUi.verify.length === 2, `Git uncertain panel must offer exactly two read-only checks: ${JSON.stringify(uncertainUi)}`)
      if (!(await cdp.eval('Boolean(document.querySelector("[data-testid=bottom-task-list]"))'))) {
        await cdp.eval('document.querySelector("[data-testid=status-tasks]")?.click()')
      }
      await waitFor(() => cdp.eval(`Boolean(document.querySelector('[data-testid=bottom-task-list] [title="${timeoutCommit.task_id}"]'))`), 15000, 'uncertain task in bottom overview')
      const bottomUncertain = await waitFor(() => cdp.eval(`(() => {
        const row = document.querySelector('[data-testid=bottom-task-list] [title="${timeoutCommit.task_id}"]')?.parentElement
        const badge = row?.querySelector('.bottom-panel-task-uncertain')
        return row && row.dataset.state === 'failed' && badge ? { state: row.dataset.state, badge: badge.innerText } : null
      })()`), 15000, 'uncertain badge in bottom overview')
      check(bottomUncertain.badge.trim().length > 0, `bottom uncertain badge is empty: ${JSON.stringify(bottomUncertain)}`)
      const m3Shot = await cdp.send('Page.captureScreenshot', { format: 'png' })
      writeFileSync(path.join(results, 'git-write-m3-uncertain.png'), Buffer.from(m3Shot.data, 'base64'))
      await sleep(1500)
      const afterTimeout = await tasks()
      check(afterTimeout.filter((item) => item.action === 'commit').length === commitCountBeforeTimeout + 1,
        'timeout commit triggered an extra commit')
      check(afterTimeout.filter((item) => item.action === 'init').length === initCountBeforeTimeout,
        'timeout commit triggered an automatic init')
      check(afterTimeout.filter((item) => item.task_id === timeoutCommit.task_id).length === 1,
        'timeout commit was duplicated')
      writeFileSync(path.join(results, 'git-write.m3.json'), `${JSON.stringify({ task: timeoutCommit, ui: uncertainUi, bottom: bottomUncertain }, null, 2)}\n`)
      console.log('uncertain timeout: passed')
      check(session.errors.length === 0, `Git panel console errors: ${session.errors.join(' | ')}`)
    } finally { await stop(session) }
  })
}

main().catch((error) => {
  console.error(error.stack || error.message)
  process.exitCode = 1
})
