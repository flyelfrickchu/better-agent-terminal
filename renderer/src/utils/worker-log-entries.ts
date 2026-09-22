// Bounded in-memory worker log for WorkerPanel.
//
// The panel keeps a copy of every output entry so it can re-render the
// terminal when process visibility changes. The Rust-side scrollback store
// caps each panel at 1 MiB; mirror that cap here so the renderer copy cannot
// grow without limit while a worker streams a long build log.

export interface WorkerLogEntry {
  name: string
  color: string
  data: string
}

export const WORKER_LOG_MAX_BYTES = 1 << 20 // 1 MiB, matches worker_buffer.rs

export class WorkerLogStore {
  private list: WorkerLogEntry[] = []
  private total = 0

  constructor(private readonly maxBytes: number = WORKER_LOG_MAX_BYTES) {}

  get entries(): WorkerLogEntry[] {
    return this.list
  }

  get bytes(): number {
    return this.total
  }

  push(entry: WorkerLogEntry): void {
    this.list.push(entry)
    this.total += entry.data.length
    this.evict()
  }

  replace(entries: WorkerLogEntry[]): void {
    this.list = entries.slice()
    this.total = 0
    for (const entry of this.list) this.total += entry.data.length
    this.evict()
  }

  clear(): void {
    this.list = []
    this.total = 0
  }

  private evict(): void {
    // Drop oldest entries until under the cap, but always keep the newest one.
    let dropped = 0
    while (this.total > this.maxBytes && this.list.length - dropped > 1) {
      this.total -= this.list[dropped].data.length
      dropped++
    }
    if (dropped > 0) this.list.splice(0, dropped)
  }
}
