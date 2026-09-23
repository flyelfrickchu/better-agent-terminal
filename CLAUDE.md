# CLAUDE.md - Project Guidelines

## No Regressions Policy

- **NEVER** break existing features when implementing new ones.
- Before committing, verify ALL existing features still work — not just the new changes.
- Run the build (`pnpm run compile`) to confirm compilation succeeds.
- When modifying shared code (stores, IPC handlers, types), trace all consumers to ensure nothing breaks.

## Package Management

- This repository uses **pnpm**. Do not use `npm install`, `npm ci`, or `npx` for project workflows.
- The pinned package manager is declared in `package.json` (`packageManager`: `pnpm@10.33.2`).
- Use `pnpm install --frozen-lockfile` for reproducible installs.
- Use `pnpm exec <tool>` instead of `npx <tool>`.
- Keep `pnpm-lock.yaml` committed and do not reintroduce `package-lock.json`.
- pnpm v10 blocks dependency lifecycle scripts unless explicitly allowed; required build-script packages are listed under `pnpm.onlyBuiltDependencies` in `package.json`.
- Standard verification commands:
  - `pnpm exec tsc --noEmit --pretty false`
  - `pnpm run compile`
  - `pnpm run test:sidecar`
  - `pnpm run check:tauri-rust`
  - `pnpm run test:tauri-rust`
  - For local packaging verification without macOS signing/notarization: `pnpm run tauri:build:debug`

## Logging

- **Frontend (renderer)**: Use `window.batAppAPI.debug.log(...)` instead of `console.log()`. This sends logs to the Tauri host logger, which writes to disk.
- **Backend (Tauri/Rust)**: Use the project Rust logging/debug helpers so logs are persisted.
- Do NOT use `console.log()` for debugging — use the logger so logs are persisted and visible in the log file.
- **Log file location**:
  - Tauri writes renderer/Rust logs to `<app-data>/logs/debug.log` and sidecar logs to `<app-data>/logs/sidecar.log`.
  - macOS fresh Tauri install: `~/Library/Application Support/com.tonyq.better-agent-terminal/logs/debug.log`
  - macOS existing Electron migration: `~/Library/Application Support/BetterAgentTerminal/logs/debug.log`
  - Windows fresh Tauri install: `%APPDATA%\com.tonyq.better-agent-terminal\logs\debug.log`
  - Linux fresh Tauri install: `~/.local/share/com.tonyq.better-agent-terminal/logs/debug.log` or the platform-resolved Tauri app data dir.
  - `BAT_TAURI_DATA_DIR` overrides the app data directory in dev/tests.
  - In the app, Settings → Open Logs Folder opens the active logs directory.

## Sub-agent / Active Tasks Tracking

- The Claude Agent SDK does **NOT** reliably emit `task_started` / `task_progress` / `task_notification` system messages.
- We track Agent/Task tools from `tool_use` blocks directly in `session.activeTasks` (in `claude-agent-manager.ts`).
- `stopTask()` falls back to using `toolUseId` as `task_id` when no mapping exists.
- Tool results for Agent/Task must clean up `activeTasks` entries.

## React Rendering

- Use `flushSync` from `react-dom` for Agent/Task tool state changes (`setMessages` in `onToolUse` and `onToolResult`) to prevent rendering delays from React 18 batching during streaming.
- Do NOT use `flushSync` for regular tool calls — only for state changes that affect the active tasks bar visibility.

## Status Line

- Our status line implementation is superior to external alternatives (e.g., ccstatusline). Do not replace it.
- 15 configurable items (see `STATUSLINE_ITEMS` in `renderer/src/types/index.ts`) with custom colors, zone alignment, and template-based config.
- Usage polling: Chrome session key (primary, lenient rate limits) → OAuth fallback (strict rate limits).

## Remote State Ownership

- Remote mode is host-owned by default. Except for purely local presentation state, clients do not own remote state.
- Message filters are client-only presentation state and may stay local because they do not change host data.
- All other remote actions must be sent to the host, and the client should render the host response or host broadcast reflection.
- When a remote client requests a mutation, the host applies or rejects it first, then returns the result and broadcasts canonical shared-state changes to every connected client.
- Avoid client-side optimistic final state for remote workflows. Loading/pending UI is fine while waiting for host confirmation.

## Git Workflow

- Do **NOT** auto-create a branch before committing. Commit directly to the current branch (including `main`) unless the user explicitly asks for a branch or PR. This overrides the default "branch first on the default branch" behavior.
- Still only commit/push when the user asks.

## Release

- 發版流程（預覽版 / 正式版 tag 規則）在 `.claude/skills/release/SKILL.md`；聽到「發布測試版」「發正式」「release new pre tag version」等說法時載入該 skill 並直接執行。

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
