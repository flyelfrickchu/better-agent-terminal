import { accessSync, constants, readFileSync, realpathSync, statSync } from 'node:fs'
import { delimiter, join, resolve } from 'node:path'
import { homedir } from 'node:os'
import { resolveDataDir } from './data-paths.mjs'

// Return a per-session snapshot; never change the shared sidecar's process.env.
export function loadAgentPythonEnvironment(cwd, { dataDir = resolveDataDir(), baseEnv = process.env } = {}) {
  let settings
  try {
    settings = JSON.parse(readFileSync(join(dataDir, 'settings.json'), 'utf8'))
  } catch (err) {
    if (err.code === 'ENOENT') return undefined
    throw new Error(`Cannot read Python virtual environment settings: ${err.message}`)
  }
  if (settings?.agentPythonVenvEnabled !== true) return undefined
  const configured = typeof settings.agentPythonVenvPath === 'string' ? settings.agentPythonVenvPath.trim() : ''
  if (!configured) throw new Error('Set the Python virtual environment path in Settings → Agent before starting an agent.')
  const expanded = configured.startsWith('~/') || configured.startsWith('~\\')
    ? join(homedir(), configured.slice(2)) : configured
  let root
  let bin
  try {
    root = realpathSync(resolve(cwd, expanded))
    if (!statSync(join(root, 'pyvenv.cfg')).isFile()) throw new Error('missing pyvenv.cfg')
    bin = join(root, process.platform === 'win32' ? 'Scripts' : 'bin')
    const python = join(bin, process.platform === 'win32' ? 'python.exe' : 'python')
    if (!statSync(python).isFile()) throw new Error('missing Python executable')
    accessSync(python, process.platform === 'win32' ? constants.F_OK : constants.X_OK)
  } catch (err) {
    throw new Error(`Invalid Python virtual environment "${configured}": ${err.message}`)
  }
  const env = { ...baseEnv }
  const pathKey = Object.keys(env).find(key => key.toUpperCase() === 'PATH') || 'PATH'
  env[pathKey] = bin + (env[pathKey] ? delimiter + env[pathKey] : '')
  env.VIRTUAL_ENV = root
  for (const key of Object.keys(env)) {
    if (key.toUpperCase() === 'PYTHONHOME') delete env[key]
  }
  return env
}
