import assert from 'node:assert/strict'
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, delimiter } from 'node:path'
import { loadAgentPythonEnvironment } from '../src/lib/python-venv.mjs'

const root = mkdtempSync(join(tmpdir(), 'bat-python-venv-'))
try {
  const dataDir = join(root, 'data')
  const cwd = join(root, 'workspace with spaces')
  const venv = join(cwd, '.venv')
  const bin = join(venv, process.platform === 'win32' ? 'Scripts' : 'bin')
  mkdirSync(dataDir)
  mkdirSync(bin, { recursive: true })
  writeFileSync(join(venv, 'pyvenv.cfg'), 'home = /python\n')
  writeFileSync(join(bin, process.platform === 'win32' ? 'python.exe' : 'python'), '', { mode: 0o755 })
  const baseEnv = { PATH: '/original/bin', PYTHONHOME: '/wrong/python', KEEP_ME: 'yes' }
  const load = () => loadAgentPythonEnvironment(cwd, { dataDir, baseEnv })
  const settings = (enabled, path) => writeFileSync(join(dataDir, 'settings.json'), JSON.stringify({
    agentPythonVenvEnabled: enabled, agentPythonVenvPath: path,
  }))
  assert.equal(load(), undefined, 'missing settings leave the environment unchanged')
  settings(false, '/missing')
  assert.equal(load(), undefined, 'disabled setting must not validate a stale path')
  settings(true, '.venv')
  const activated = load()
  assert.equal(activated?.VIRTUAL_ENV, venv)
  assert.equal(activated.PATH, `${bin}${delimiter}${baseEnv.PATH}`)
  assert.equal(activated.KEEP_ME, 'yes')
  assert.equal(activated.PYTHONHOME, undefined)
  assert.equal(baseEnv.PYTHONHOME, '/wrong/python', 'activation must not mutate shared environment')
  settings(true, venv)
  assert.deepEqual(load(), activated, 'absolute and workspace-relative paths match')
  settings(true, '')
  assert.throws(load, /virtual environment path/i)
  settings(true, 'missing')
  assert.throws(load, /virtual environment/i)
  settings(true, '.venv')
  rmSync(join(bin, process.platform === 'win32' ? 'python.exe' : 'python'))
  assert.throws(load, /python/i)
  assert.equal(activated.VIRTUAL_ENV, venv, 'existing sessions retain their environment snapshot')
  console.log('Python virtual environment tests passed')
} finally {
  rmSync(root, { recursive: true, force: true })
}
