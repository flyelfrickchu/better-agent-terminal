import fs from 'node:fs'
import path from 'node:path'
import { createHash } from 'node:crypto'
import { fileURLToPath } from 'node:url'

const sourceDirectories = ['qt-ui/src', 'src-tauri/src', 'node-sidecar/src', 'shared']
const sourceFiles = [
  'qt-ui/CMakeLists.txt', 'qt-ui/better-agent-terminal-qt.desktop',
  'scripts/qt-version.mjs', 'scripts/build-qt.mjs', 'scripts/package-qt-appimage.mjs',
  'src-tauri/Cargo.toml', 'src-tauri/Cargo.lock', 'src-tauri/build.rs',
  'node-sidecar/package.json', 'node-sidecar/pnpm-lock.yaml',
  'package.json', 'pnpm-lock.yaml', 'runtime-catalog.json', 'assets/icon.png',
]

export function incrementQtVersion(version) {
  const match = /^(0|[1-9]\d*)\.([0-9])\.([0-9])$/.exec(version)
  if (!match) throw new Error(`Invalid Qt version: ${version}; expected major.0-9.0-9`)
  const next = BigInt(match[1]) * 100n + BigInt(match[2]) * 10n + BigInt(match[3]) + 1n
  return `${next / 100n}.${next / 10n % 10n}.${next % 10n}`
}

export function versionInputPaths(root) {
  const files = new Set(sourceFiles.filter(file => fs.existsSync(path.join(root, file))))
  function walk(relative) {
    const directory = path.join(root, relative)
    if (!fs.existsSync(directory)) return
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const child = `${relative}/${entry.name}`
      if (entry.isDirectory()) walk(child)
      else if (entry.isFile()) files.add(child)
    }
  }
  sourceDirectories.forEach(walk)
  return [...files].sort()
}

export function sourceFingerprint(root) {
  const hash = createHash('sha256')
  for (const file of versionInputPaths(root))
    hash.update(file).update('\0').update(fs.readFileSync(path.join(root, file))).update('\0')
  return hash.digest('hex')
}

function readState(root) {
  const state = JSON.parse(fs.readFileSync(path.join(root, 'qt-ui/version.json'), 'utf8'))
  incrementQtVersion(state.version) // Validate before touching generated files.
  if (state.sourceHash !== null && !/^[a-f0-9]{64}$/.test(state.sourceHash))
    throw new Error('Invalid Qt version sourceHash')
  return state
}

function writeIfChanged(file, contents) {
  if (fs.existsSync(file) && fs.readFileSync(file, 'utf8') === contents) return
  fs.mkdirSync(path.dirname(file), { recursive: true })
  const temporary = `${file}.${process.pid}.tmp`
  try {
    fs.writeFileSync(temporary, contents)
    fs.renameSync(temporary, file)
  } finally {
    fs.rmSync(temporary, { force: true })
  }
}

export function prepareQtVersion(root, buildDir) {
  const current = readState(root)
  const sourceHash = sourceFingerprint(root)
  const proposed = {
    version: current.sourceHash === sourceHash ? current.version : incrementQtVersion(current.version),
    sourceHash,
    previousVersion: current.version,
    previousSourceHash: current.sourceHash,
  }
  writeIfChanged(path.join(buildDir, 'bat-qt-version.h'), `#pragma once\n#define BAT_QT_VERSION "${proposed.version}"\n`)
  writeIfChanged(path.join(buildDir, 'bat-qt-version.json'), JSON.stringify(proposed, null, 2) + '\n')
  return proposed
}

export function commitQtVersion(root, buildDir) {
  const proposed = JSON.parse(fs.readFileSync(path.join(buildDir, 'bat-qt-version.json'), 'utf8'))
  incrementQtVersion(proposed.version)
  const lock = path.join(root, 'qt-ui/.version.lock')
  const fd = fs.openSync(lock, 'wx')
  try {
    if (sourceFingerprint(root) !== proposed.sourceHash)
      throw new Error('Qt sources changed during compilation; rebuild before committing the version')
    const current = readState(root)
    const alreadyCommitted = current.version === proposed.version && current.sourceHash === proposed.sourceHash
    if (!alreadyCommitted && (current.version !== proposed.previousVersion || current.sourceHash !== proposed.previousSourceHash))
      throw new Error('Qt version conflict with another build; configure and rebuild again')
    const next = { version: proposed.version, sourceHash: proposed.sourceHash }
    const binaryHash = createHash('sha256').update(fs.readFileSync(path.join(buildDir, 'better-agent-terminal-qt'))).digest('hex')
    // A configure proposal is not evidence that this directory linked a binary.
    // Keep a separate receipt tied to the actual successful executable.
    writeIfChanged(path.join(buildDir, 'bat-qt-built.json'), JSON.stringify({ ...next, binaryHash }, null, 2) + '\n')
    writeIfChanged(path.join(root, 'qt-ui/version.json'), JSON.stringify(next, null, 2) + '\n')
    return next
  } finally {
    fs.closeSync(fd)
    fs.unlinkSync(lock)
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const [command, root, buildDir] = process.argv.slice(2)
    if (!root || !buildDir || !['prepare', 'commit'].includes(command))
      throw new Error('Usage: node scripts/qt-version.mjs prepare|commit ROOT BUILD_DIR')
    const state = command === 'prepare' ? prepareQtVersion(root, buildDir) : commitQtVersion(root, buildDir)
    process.stdout.write(JSON.stringify(state) + '\n')
  } catch (error) {
    process.stderr.write(`Qt version: ${error.message}\n`)
    process.exitCode = 1
  }
}
