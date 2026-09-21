import assert from 'node:assert/strict'
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

const root = mkdtempSync(join(tmpdir(), 'bat-venv-session-'))
process.env.BAT_SIDECAR_DATA_DIR = root
const { dispatch } = await import('../src/server.mjs')
const { sessions } = await import('../src/lib/state.mjs')
const { __setSdkOverrideForTests } = await import('../src/lib/sdk-loader.mjs')
let nextId = 1
const send = (method, params) => dispatch({ jsonrpc: '2.0', id: nextId++, method, params })
const cwd = join(root, 'workspace')
const venv = join(cwd, '.venv')
const bin = join(venv, process.platform === 'win32' ? 'Scripts' : 'bin')
mkdirSync(bin, { recursive: true })
writeFileSync(join(venv, 'pyvenv.cfg'), 'home = /python\n')
writeFileSync(join(bin, process.platform === 'win32' ? 'python.exe' : 'python'), '', { mode: 0o755 })
const settings = (enabled, path) => writeFileSync(join(root, 'settings.json'), JSON.stringify({
  agentPythonVenvEnabled: enabled, agentPythonVenvPath: path,
}))
const captured = []
__setSdkOverrideForTests({
  query({ options }) {
    captured.push(options)
    return (async function* () {
      yield { type: 'result', subtype: 'success', result: 'done', session_id: 'venv-sdk-test', duration_ms: 1, num_turns: 1, is_error: false }
    })()
  },
})
try {
  settings(true, 'missing')
  const invalid = await send('claude.startSession', { sessionId: 'invalid', options: { cwd } })
  assert.match(invalid.error?.message || '', /Invalid Python virtual environment/)
  assert.equal(sessions.has('invalid'), false, 'invalid environment must block session creation')

  settings(true, '.venv')
  const started = await send('claude.startSession', { sessionId: 'enabled', options: { cwd, autoCompactWindow: 50000 } })
  assert.equal(started.result?.ok, true, JSON.stringify(started))
  assert.equal(sessions.get('enabled').pythonVenvEnv.VIRTUAL_ENV, venv)

  settings(false, 'missing')
  const disabled = await send('claude.startSession', { sessionId: 'disabled', options: { cwd } })
  assert.equal(disabled.result?.ok, true)
  assert.equal(sessions.get('disabled').pythonVenvEnv, undefined)
  assert.equal(sessions.get('enabled').pythonVenvEnv.VIRTUAL_ENV, venv, 'changing settings must not mutate an existing session')

  const reply = await send('claude.sendMessage', { sessionId: 'enabled', prompt: 'test environment' })
  assert.equal(reply.error, undefined, JSON.stringify(reply))
  assert.equal(captured.length, 1)
  assert.equal(captured[0].env.VIRTUAL_ENV, venv, 'SDK subprocess must receive the selected environment')
  assert.equal(captured[0].env.PYTHONHOME, undefined)
  assert.equal(captured[0].env.CLAUDE_CODE_AUTO_COMPACT_WINDOW, '50000', 'compaction settings must not overwrite activation')
  for (let i = 0; i < 20 && sessions.get('enabled')?.streaming; i++) {
    await new Promise(resolve => setImmediate(resolve))
  }
  console.log('Python virtual environment session tests passed')
} finally {
  await send('claude.stopSession', { sessionId: 'enabled' })
  await send('claude.stopSession', { sessionId: 'disabled' })
  __setSdkOverrideForTests(undefined)
  rmSync(root, { recursive: true, force: true })
}
