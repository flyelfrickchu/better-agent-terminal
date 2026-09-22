import assert from 'node:assert/strict'
import {
  appendTerminalPreview,
  PREVIEW_MAX_LINES,
  PREVIEW_MAX_LINE_CHARS,
} from '../renderer/src/utils/terminal-preview'
import { WorkerLogStore, WORKER_LOG_MAX_BYTES } from '../renderer/src/utils/worker-log-entries'

// --- TerminalThumbnail preview cache -------------------------------------

{
  // Progress bars rewrite one line with \r and never emit \n. The preview
  // must stay bounded no matter how many such chunks arrive.
  const frame = ('\r\x1b[2K  Compiling foo [=====>    ] 45/120: 37%').repeat(1200)
  let preview = ''
  for (let i = 0; i < 200; i++) preview = appendTerminalPreview(preview, frame)
  const bound = PREVIEW_MAX_LINES * PREVIEW_MAX_LINE_CHARS + PREVIEW_MAX_LINES
  assert.ok(
    preview.length <= bound,
    `CR-only output must be bounded: got ${preview.length} > ${bound}`,
  )
  assert.ok(preview.endsWith('37%'), 'preview keeps the tail of the overwritten line')
}

{
  // Normal newline-terminated logs keep only the last PREVIEW_MAX_LINES lines.
  let preview = ''
  for (let i = 0; i < 50; i++) preview = appendTerminalPreview(preview, `line ${i}\n`)
  const lines = preview.split('\n')
  assert.equal(lines.length, PREVIEW_MAX_LINES)
  assert.equal(lines[lines.length - 1], '')
  assert.equal(lines[lines.length - 2], 'line 49')
}

{
  // Work per chunk must not scale with total output seen so far.
  const chunk = ('\rprogress ' + 'x'.repeat(50)).repeat(1000)
  let preview = ''
  for (let i = 0; i < 20; i++) preview = appendTerminalPreview(preview, chunk)
  const t0 = performance.now()
  for (let i = 0; i < 200; i++) preview = appendTerminalPreview(preview, chunk)
  const elapsed = performance.now() - t0
  assert.ok(elapsed < 2000, `200 chunks took ${elapsed.toFixed(0)}ms; expected bounded work per chunk`)
}

{
  // ANSI escapes and carriage returns are stripped as before.
  const preview = appendTerminalPreview('', '\x1b[32mok\x1b[0m\r\n\x1b]0;title\x07done\n')
  assert.equal(preview, 'ok\ndone\n')
}

// --- WorkerPanel in-memory log -------------------------------------------

{
  const store = new WorkerLogStore(100)
  for (let i = 0; i < 20; i++) {
    store.push({ name: 'web', color: '32', data: `${String(i).padStart(2, '0')}${'.'.repeat(8)}\n` })
  }
  assert.ok(store.bytes <= 100, `bytes ${store.bytes} exceeds cap`)
  const entries = store.entries
  assert.ok(entries.length > 0 && entries.length < 20, 'oldest entries evicted')
  assert.equal(entries[entries.length - 1].data.slice(0, 2), '19', 'newest entry retained')
  assert.equal(entries[0].data.slice(0, 2), String(20 - entries.length).padStart(2, '0'), 'contiguous tail retained')
}

{
  // replace() and clear() reset the byte accounting.
  const store = new WorkerLogStore(50)
  store.push({ name: 'a', color: '', data: 'x'.repeat(30) })
  store.replace([{ name: 'b', color: '', data: 'y'.repeat(10) }])
  assert.equal(store.bytes, 10)
  assert.equal(store.entries.length, 1)
  store.clear()
  assert.equal(store.bytes, 0)
  assert.equal(store.entries.length, 0)
}

{
  // A single oversized entry is still kept (never lose the newest chunk).
  const store = new WorkerLogStore(10)
  store.push({ name: 'a', color: '', data: 'z'.repeat(40) })
  assert.equal(store.entries.length, 1)
}

assert.equal(WORKER_LOG_MAX_BYTES, 1 << 20)

console.log('terminal-output-memory tests passed')
