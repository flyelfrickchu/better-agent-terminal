import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const modulePath = new URL('../scripts/qt-version.mjs', import.meta.url)
const { incrementQtVersion, prepareQtVersion, commitQtVersion, sourceFingerprint } = await import(modulePath)

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'bat-qt-version-'))
  t.after(() => fs.rmSync(root, { recursive: true, force: true }))
  fs.mkdirSync(path.join(root, 'qt-ui/src'), { recursive: true })
  fs.writeFileSync(path.join(root, 'qt-ui/src/main.cpp'), 'int main() { return 0; }\n')
  fs.writeFileSync(path.join(root, 'qt-ui/version.json'), JSON.stringify({ version: '0.1.0', sourceHash: null }))
  const buildDir = path.join(root, 'build')
  fs.mkdirSync(buildDir)
  fs.writeFileSync(path.join(buildDir, 'better-agent-terminal-qt'), 'linked Qt executable fixture')
  return { root, buildDir }
}

test('decimal carry follows the requested three-part format', () => {
  for (const [before, after] of [['0.1.0', '0.1.1'], ['0.0.9', '0.1.0'], ['0.9.9', '1.0.0'], ['9.9.9', '10.0.0']])
    assert.equal(incrementQtVersion(before), after)
  for (const bad of ['0.1.10', '0.10.0', '1.2', '-1.0.0', '01.2.3', '1.2.3-dev'])
    assert.throws(() => incrementQtVersion(bad), /version/i)
})

test('only a successful build commits the version, with no bump for identical sources', t => {
  const { root, buildDir } = fixture(t)
  const proposed = prepareQtVersion(root, buildDir)
  assert.equal(proposed.version, '0.1.1')
  assert.equal(JSON.parse(fs.readFileSync(path.join(root, 'qt-ui/version.json'))).version, '0.1.0')
  assert.equal(prepareQtVersion(root, buildDir).version, '0.1.1', 'failed build retry reuses proposed number')
  commitQtVersion(root, buildDir)
  assert.equal(prepareQtVersion(root, buildDir).version, '0.1.1')
  commitQtVersion(root, buildDir)
  fs.appendFileSync(path.join(root, 'qt-ui/src/main.cpp'), '// changed feature\n')
  assert.equal(prepareQtVersion(root, buildDir).version, '0.1.2')
})

test('source hashes include added/deleted runtime files and ignore docs, tests and generated versions', t => {
  const { root, buildDir } = fixture(t)
  const original = sourceFingerprint(root)
  fs.writeFileSync(path.join(root, 'qt-ui/README.md'), 'New docs')
  fs.mkdirSync(path.join(root, 'qt-ui/tests'))
  fs.writeFileSync(path.join(root, 'qt-ui/tests/core_test.cpp'), 'New test')
  prepareQtVersion(root, buildDir)
  assert.equal(sourceFingerprint(root), original)
  const added = path.join(root, 'qt-ui/src/feature.cpp')
  fs.writeFileSync(added, 'void feature() {}')
  assert.notEqual(sourceFingerprint(root), original)
  fs.unlinkSync(added)
  assert.equal(sourceFingerprint(root), original)
  fs.mkdirSync(path.join(root, 'src-tauri/src'), { recursive: true })
  fs.writeFileSync(path.join(root, 'src-tauri/src/feature.rs'), 'fn feature() {}')
  assert.notEqual(sourceFingerprint(root), original)
})

test('commit refuses edited sources and conflicting build proposals', t => {
  const { root, buildDir } = fixture(t)
  prepareQtVersion(root, buildDir)
  fs.appendFileSync(path.join(root, 'qt-ui/src/main.cpp'), '// changed during build\n')
  assert.throws(() => commitQtVersion(root, buildDir), /changed/i)
  const otherBuild = path.join(root, 'build2')
  prepareQtVersion(root, otherBuild)
  fs.writeFileSync(path.join(otherBuild, 'better-agent-terminal-qt'), 'other linked executable')
  commitQtVersion(root, otherBuild)
  assert.throws(() => commitQtVersion(root, buildDir), /changed|conflict/i)
  assert.equal(JSON.parse(fs.readFileSync(path.join(root, 'qt-ui/version.json'))).version, '0.1.1')
})

test('identical prepare leaves the generated header timestamp intact', t => {
  const { root, buildDir } = fixture(t)
  prepareQtVersion(root, buildDir)
  const header = path.join(buildDir, 'bat-qt-version.h')
  fs.utimesSync(header, 1000, 1000)
  prepareQtVersion(root, buildDir)
  assert.equal(fs.statSync(header).mtimeMs, 1000000)
})
