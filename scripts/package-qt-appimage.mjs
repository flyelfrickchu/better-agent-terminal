import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { fileURLToPath } from 'node:url'
import { incrementQtVersion, sourceFingerprint } from './qt-version.mjs'

export function appImageName(version, architecture = process.arch) {
  incrementQtVersion(version)
  const arch = { x64: 'amd64', arm64: 'aarch64' }[architecture]
  if (!arch) throw new Error(`Unsupported AppImage architecture: ${architecture}`)
  return `BetterAgentTerminalQt_${version}-dev_${arch}.AppImage`
}

export function validateQtBuild(root, buildDir, expected = null) {
  const built = JSON.parse(fs.readFileSync(path.join(buildDir, 'bat-qt-built.json'), 'utf8'))
  const committed = JSON.parse(fs.readFileSync(path.join(root, 'qt-ui/version.json'), 'utf8'))
  incrementQtVersion(built.version)
  if (built.version !== committed.version || built.sourceHash !== committed.sourceHash)
    throw new Error('Qt version has no matching successful build; build the Qt executable first')
  if (built.sourceHash !== sourceFingerprint(root))
    throw new Error('Qt sources changed since compilation; rebuild before packaging')
  const binaryHash = createHash('sha256').update(fs.readFileSync(path.join(buildDir, 'better-agent-terminal-qt'))).digest('hex')
  if (binaryHash !== built.binaryHash) throw new Error('Qt executable does not match its successful build receipt')
  if (expected && ['version', 'sourceHash', 'binaryHash'].some(key => built[key] !== expected[key]))
    throw new Error('Qt build changed during AppImage packaging; retry with the new build')
  return built
}

function run(program, args, options = {}) {
  const result = spawnSync(program, args, { stdio: 'inherit', ...options })
  if (result.error) throw result.error
  if (result.status !== 0) throw new Error(`${program} failed (${result.status ?? result.signal})`)
  return result.stdout
}

function cloneAppDir(source, target) {
  fs.mkdirSync(target, { recursive: true })
  for (const entry of fs.readdirSync(source, { withFileTypes: true })) {
    const src = path.join(source, entry.name), dest = path.join(target, entry.name)
    if (entry.isDirectory()) cloneAppDir(src, dest)
    else if (entry.isSymbolicLink()) fs.symlinkSync(fs.readlinkSync(src), dest)
    else if (entry.isFile()) {
      try { fs.linkSync(src, dest) }
      catch (error) {
        if (error.code !== 'EXDEV') throw error
        fs.copyFileSync(src, dest)
      }
    }
  }
}

function appendFile(fd, source) {
  const input = fs.openSync(source, 'r')
  try {
    const buffer = Buffer.alloc(1024 * 1024)
    let bytes
    while ((bytes = fs.readSync(input, buffer, 0, buffer.length, null)) > 0)
      fs.writeFileSync(fd, buffer.subarray(0, bytes))
  } finally { fs.closeSync(input) }
}

export function packageQtAppImage(root, buildDir, seedAppDir, runtimeImage) {
  const built = validateQtBuild(root, buildDir)
  const release = path.join(root, 'release')
  fs.mkdirSync(release, { recursive: true })
  const output = path.join(release, appImageName(built.version))
  if (fs.existsSync(output)) throw new Error(`AppImage already exists: ${output}; refusing to overwrite`)
  if (!fs.existsSync(path.join(seedAppDir, 'AppRun')))
    throw new Error(`Prepared Qt/KDE AppDir is required: ${seedAppDir}`)
  if (!runtimeImage) {
    const candidates = fs.readdirSync(release)
      .filter(name => /^BetterAgentTerminalQt_.*\.AppImage$/.test(name))
      .map(name => path.join(release, name))
      .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)
    runtimeImage = candidates[0]
  }
  if (!runtimeImage) throw new Error('Supply an existing Qt AppImage with --runtime-from to reuse its type-2 runtime')
  const env = { ...process.env, QT_QPA_PLATFORM: 'offscreen' }
  for (const key of ['LD_LIBRARY_PATH', 'PYTHONHOME', 'PYTHONPATH']) delete env[key]
  const offset = Number(run(runtimeImage, ['--appimage-offset'], { env, encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] }).trim())
  if (!Number.isSafeInteger(offset) || offset <= 0 || offset >= fs.statSync(runtimeImage).size)
    throw new Error('Invalid AppImage runtime offset')
  const runtime = Buffer.alloc(offset)
  const runtimeFd = fs.openSync(runtimeImage, 'r')
  try {
    if (fs.readSync(runtimeFd, runtime, 0, offset, 0) !== offset || runtime.subarray(0, 4).toString('hex') !== '7f454c46'
      || runtime.subarray(8, 11).toString('hex') !== '414902')
      throw new Error('Expected a type-2 ELF AppImage runtime')
  } finally { fs.closeSync(runtimeFd) }
  const staging = fs.mkdtempSync(path.join(buildDir, 'appimage-'))
  const appDir = path.join(staging, 'AppDir')
  const temporaryOutput = path.join(staging, 'output.AppImage')
  try {
    cloneAppDir(seedAppDir, appDir)
    const ui = path.join(appDir, 'usr/bin/better-agent-terminal-qt')
    fs.unlinkSync(ui) // Break the hard link before patching the new executable.
    fs.copyFileSync(path.join(buildDir, 'better-agent-terminal-qt'), ui)
    if (createHash('sha256').update(fs.readFileSync(ui)).digest('hex') !== built.binaryHash)
      throw new Error('Qt executable changed while staging the AppImage')
    fs.chmodSync(ui, 0o755)
    run('patchelf', ['--set-rpath', '$ORIGIN/../lib', ui], { env })
    const versionOutput = run(path.join(appDir, 'AppRun'), ['--version'], { env, encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] }).trim()
    if (versionOutput !== `better-agent-terminal-qt ${built.version}-dev`)
      throw new Error(`Packaged executable version mismatch: ${versionOutput}`)
    const metadata = path.join(appDir, 'usr/share/bat-qt-build.json')
    fs.rmSync(metadata, { force: true })
    fs.writeFileSync(metadata, JSON.stringify({ version: built.version, sourceHash: built.sourceHash }, null, 2) + '\n')
    const squashfs = path.join(staging, 'payload.squashfs')
    run('mksquashfs', [appDir, squashfs, '-noappend', '-comp', 'zstd', '-processors', '4', '-no-progress'], { env })
    const fd = fs.openSync(temporaryOutput, 'wx', 0o755)
    try { fs.writeFileSync(fd, runtime); appendFile(fd, squashfs) }
    finally { fs.closeSync(fd) }
    // Recheck in case source files were edited during compression.
    validateQtBuild(root, buildDir, built)
    // Publish only a complete image, without replacing an existing release.
    const publishing = path.join(release, `.${path.basename(output)}.${process.pid}.tmp`)
    try {
      fs.copyFileSync(temporaryOutput, publishing, fs.constants.COPYFILE_EXCL)
      fs.chmodSync(publishing, 0o755)
      fs.linkSync(publishing, output)
    } finally { fs.rmSync(publishing, { force: true }) }
    const hash = createHash('sha256')
    const file = fs.openSync(output, 'r')
    try {
      const buffer = Buffer.alloc(1024 * 1024)
      let bytes
      while ((bytes = fs.readSync(file, buffer, 0, buffer.length, null)) > 0) hash.update(buffer.subarray(0, bytes))
    } finally { fs.closeSync(file) }
    const digest = hash.digest('hex')
    fs.writeFileSync(output + '.sha256', `${digest}  ${path.basename(output)}\n`)
    console.log(`AppImage: ${output}\nVersion: ${built.version}-dev\nSHA-256: ${digest}`)
    return output
  } finally { fs.rmSync(staging, { recursive: true, force: true }) }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const root = fileURLToPath(new URL('../', import.meta.url))
    const options = { '--build-dir': 'qt-ui/build', '--appdir': 'release/BetterAgentTerminalQt.AppDir', '--runtime-from': null }
    for (let i = 2; i < process.argv.length; i += 2) {
      const flag = process.argv[i], value = process.argv[i + 1]
      if (!(flag in options) || !value) throw new Error(`Expected --build-dir DIR, --appdir DIR or --runtime-from IMAGE; got ${flag}`)
      options[flag] = value
    }
    packageQtAppImage(root, path.resolve(root, options['--build-dir']), path.resolve(root, options['--appdir']),
      options['--runtime-from'] ? path.resolve(root, options['--runtime-from']) : null)
  } catch (error) {
    console.error(`Qt AppImage: ${error.message}`)
    process.exitCode = 1
  }
}
