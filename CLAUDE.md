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

## Project Memory: Kubuntu 26.04 AppImage Setup (2026-10-03)

- Verified on Kubuntu/Ubuntu 26.04.1 in VMware with a KDE Wayland desktop: Node.js 22.22.1, pnpm 10.33.2, and Ubuntu's Rust/Cargo 1.93.1 successfully built `./rebuild.sh`. Install project dependencies with `pnpm install --frozen-lockfile` first.
- Linux prerequisites used: `build-essential pkg-config python3 curl ca-certificates libssl-dev libwebkit2gtk-4.1-dev libgtk-3-dev libayatana-appindicator3-dev librsvg2-dev patchelf xdg-utils librsvg2-bin libgdk-pixbuf2.0-bin libglib2.0-bin libgtk-3-bin libfuse2t64`, plus Node.js and Rust/Cargo. Corepack activated the pinned pnpm and installed its command shims in `/usr/local/bin`.
- **Packaging failure:** the original `codex-runtime/bin/codex --version` worked, but the copy under `BetterAgentTerminal.AppDir/usr/lib/BetterAgentTerminal/codex-runtime/bin/codex` segfaulted. The packaged binary had a different SHA-256 and an added `$ORIGIN` RUNPATH. Packaged ripgrep also segfaulted. Do not assume static PIE binaries remain functional after linuxdeploy modifies them, or that `NO_STRIP=true` alone prevents RUNPATH changes.
- **Verified local repair:** after linuxdeploy, restore `bin/codex`, `bin/codex-code-mode-host`, and `codex-path/rg` in the AppDir from the original `codex-runtime` files, then repack the AppImage. Codex and ripgrep ran after restoration; the repaired AppImage's Codex hash matched the original. This repaired build artifacts and the installed AppImage; the repository's build scripts have **not** been updated to automate this repair. A fresh rebuild needs verification again.
- **Separate rendering failure:** even with repaired binaries, the window was blank/frozen and logged `[render-watchdog] rAF stalled` after about six seconds. Launching with `WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1` fixed rendering on this machine. The user confirmed normal operation; folder selection, terminal startup, and agent-session calls also succeeded. Keep these settings scoped to this application's launcher, rather than applying them globally or assuming every Linux machine needs them.
- Installed user launcher: `~/.local/share/applications/better-agent-terminal.desktop`, with `Categories=Development;` and `Exec=/usr/bin/env WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 /home/kpos/Applications/BetterAgentTerminal_0.0.1-dev_amd64.AppImage`. Validate with `desktop-file-validate` and refresh KDE with `kbuildsycoca6 --noincremental`. The AppImage must be executable. Adapt the username/path for other machines.
- Diagnose startup with stderr, `journalctl --user`, and `coredumpctl info`, as renderer startup logs alone did not reveal the Codex crash. This installation's logs are in `~/.local/share/org.tonyq.better-agent-terminal/logs/`. TypeScript, the frontend build, and sidecar tests passed during setup; the sidecar tests needed permission to create temporary fixtures under the user home directory.

## Project Memory: Qt/KDE Migration and Files Flicker Fix (2026-10-03)

- The `dev_qt` branch is migrating the React UI to native Qt 6/KDE Frameworks 6 under `qt-ui/`. KDE Konsole is the default local working terminal, embedded through its KPart; Konsole owns its shell and PTY. Agent sessions use the existing BAT host protocol and profile contexts. Full React feature parity remains unfinished; preserve the built-in configurable status line and existing IPC contracts.
- Native workflow: `pnpm run qt:build` builds the headless Rust host and Qt UI; `pnpm run qt:start` launches them; `pnpm run qt:test` runs the Qt tests. Required development packages include `cmake qt6-base-dev qt6-websockets-dev libkf6parts-dev libkf6coreaddons-dev`; embedded terminals require `konsole-kpart`.
- **Files-pane flicker: fixed and confirmed by the user.** The cause was a feedback loop: `saveSnapshot()` applied its response through `applySnapshot()`, which called `showWorkspace()`, scheduled another save, and repeatedly cleared/refetched the file tree.
- `applySnapshot()` now calls `showWorkspace(selected, false)`. Only an actual user workspace change schedules a save. An unchanged file context/directory does not trigger a refresh. Same-folder refreshes retain the visible tree until the response arrives, apply changes with painting temporarily disabled, and reject stale replies using a refresh generation counter.
- Regression coverage: `CoreTest::snapshotDoesNotScheduleAnotherSave()` verifies repeated snapshot application and reselecting the same workspace neither schedule another save nor refresh the file tree. Qt build/tests, TypeScript checking, and the frontend build passed after the fix. Konsole/TLS tests need execution outside the restricted sandbox for PTY and local socket access.

## Project Memory: Native Qt Settings (2026-10-03)

- **Settings implemented and confirmed working by the user.** The previous Qt Settings dialog was only a JSON editor and showed an effectively empty page (`{}`) on fresh installations. It is now a native `SettingsDialog` in `qt-ui/src/settingsdialog.{h,cpp}` with Appearance, Terminal, Agents, Workspaces, and Python tabs.
- Settings load/save through the existing `settings:load` / `settings:save` host contract and active profile context. Null settings represent a fresh installation with usable defaults; malformed settings disable saving. Loading, disconnection, profile changes, validation errors, and save failures are shown inside the dialog. Failed saves retain edits for retry.
- Interface theme/font preferences are stored under the settings object's `qtUi` field and apply immediately. The selected Konsole profile controls the shell, font, and colors of new terminals. Agent model/effort defaults apply to new agent tabs; workspace settings control initial terminal count and optional default agent tabs. Automatic terminal closure after process exit is supported. Python virtual environment settings use the existing backend behavior for newly started sessions.
- Native startup loads settings before restoring workspaces. Saving preserves unknown fields, unrelated host settings, and the built-in status line configuration. The settings `qtUi` field and workspace snapshot `qtUi` field belong to separate persisted objects.
- Verification passed: Qt build and tests (fresh/invalid settings, configuration preservation, TLS load/save, failed-save retry), native startup smoke test, `pnpm exec tsc --noEmit --pretty false`, and `pnpm run compile`. Remaining advanced settings and full React feature parity are still migration work; do not treat this confirmation as completion of the entire UI rewrite.

## Project Memory: Native Qt Git View and Shutdown Fix (2026-10-03)

- Git now opens inside the workspace above existing terminal/agent sessions, with commits, changed files, and Diff/File columns. Diff additions/deletions/hunks and file statuses use distinct colors. The File tab shows the current working copy; untracked text files appear as additions.
- Commit messages, paths, diffs, and file contents wrap within their column, including long strings without spaces. The Git tab has a close button; the toolbar Git button reopens it while retaining session state.
- **Shutdown segmentation fault fixed and confirmed by the user.** The core dump showed Konsole KPart destruction emitting `TerminalPanel::exited` while ancestor tabs were being destroyed. `TerminalPanel` now disconnects KPart callbacks before QWidget child teardown; the MainWindow exit callback also ignores shutdown. Normal shell exit notification remains supported.
- Verification passed: native build/tests, including workspace/app teardown with both terminal auto-close settings and normal process exit, native startup/shutdown smoke test, TypeScript checking, and the standard frontend build.
