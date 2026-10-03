import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('../', import.meta.url))
function run(command, args) {
  const result = spawnSync(command, args, { cwd: root, stdio: 'inherit' })
  if (result.error) {
    process.stderr.write(`${command}: ${result.error.message}\n`)
    process.exit(1)
  }
  if (result.status !== 0) process.exit(result.status ?? 1)
}

run('cargo', ['build', '--manifest-path', 'src-tauri/Cargo.toml', '--bin', 'bat-server',
  '--no-default-features', '--features', 'headless', '--profile', 'dev',
  '--target-dir', 'src-tauri/target/qt'])
run('cmake', ['-S', 'qt-ui', '-B', 'qt-ui/build', '-DCMAKE_BUILD_TYPE=Debug'])
run('cmake', ['--build', 'qt-ui/build', '-j2'])
