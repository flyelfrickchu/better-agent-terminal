import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { prepareQtVersion, commitQtVersion } from '../scripts/qt-version.mjs'
import { appImageName, validateQtBuild } from '../scripts/package-qt-appimage.mjs'

test('AppImage names follow the compiled Qt version and target architecture', () => {
  assert.equal(appImageName('0.1.1', 'x64'), 'BetterAgentTerminalQt_0.1.1-dev_amd64.AppImage')
  assert.equal(appImageName('1.0.0', 'arm64'), 'BetterAgentTerminalQt_1.0.0-dev_aarch64.AppImage')
  assert.throws(() => appImageName('1.0.10', 'x64'), /version/i)
  assert.throws(() => appImageName('1.0.0', 'other'), /architecture/i)
})

test('packaging rejects unbuilt, unsuccessful and stale build versions', t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bat-qt-package-version-'))
  t.after(() => fs.rmSync(root, { recursive: true, force: true }))
  const buildDir = path.join(root, 'build')
  fs.mkdirSync(path.join(root, 'qt-ui/src'), { recursive: true })
  fs.writeFileSync(path.join(root, 'qt-ui/src/main.cpp'), 'int main() {}')
  fs.writeFileSync(path.join(root, 'qt-ui/version.json'), JSON.stringify({ version: '0.1.0', sourceHash: null }))
  assert.throws(() => validateQtBuild(root, buildDir))
  prepareQtVersion(root, buildDir)
  fs.writeFileSync(path.join(buildDir, 'better-agent-terminal-qt'), 'compiled binary fixture')
  assert.throws(() => validateQtBuild(root, buildDir))
  commitQtVersion(root, buildDir)
  assert.equal(validateQtBuild(root, buildDir).version, '0.1.1')
  fs.appendFileSync(path.join(root, 'qt-ui/src/main.cpp'), '\n// feature changed')
  assert.throws(() => validateQtBuild(root, buildDir), /changed|stale/i)
})

test('configure-only receipts cannot borrow success from a different build directory', t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bat-qt-receipt-'))
  t.after(() => fs.rmSync(root, { recursive: true, force: true }))
  fs.mkdirSync(path.join(root, 'qt-ui/src'), { recursive: true })
  const source = path.join(root, 'qt-ui/src/main.cpp')
  fs.writeFileSync(source, 'source A')
  fs.writeFileSync(path.join(root, 'qt-ui/version.json'), JSON.stringify({ version: '0.1.0', sourceHash: null }))
  const buildA = path.join(root, 'build-a'), buildB = path.join(root, 'build-b')
  prepareQtVersion(root, buildA)
  fs.writeFileSync(path.join(buildA, 'better-agent-terminal-qt'), 'binary A 0.1.1')
  fs.writeFileSync(source, 'source B')
  assert.throws(() => commitQtVersion(root, buildA), /changed/i)
  prepareQtVersion(root, buildB)
  fs.writeFileSync(path.join(buildB, 'better-agent-terminal-qt'), 'binary B 0.1.1')
  commitQtVersion(root, buildB)
  prepareQtVersion(root, buildA)
  assert.throws(() => validateQtBuild(root, buildA), /ENOENT|successful|stale/i)
  const captured = validateQtBuild(root, buildB)
  fs.writeFileSync(path.join(buildB, 'better-agent-terminal-qt'), 'unrecorded replacement binary')
  assert.throws(() => validateQtBuild(root, buildB), /executable|binary/i)
  fs.writeFileSync(source, 'source C')
  prepareQtVersion(root, buildB)
  commitQtVersion(root, buildB)
  assert.throws(() => validateQtBuild(root, buildB, captured), /packaging|changed/i)
})
