# External terminal responsiveness implementation plan

> Execute inline using superpowers:executing-plans; user approved the proposed fix in chat. User subsequently requested a Git commit and project memory update; no push or external publication was requested.

**Goal:** Remove desktop-wide X11 scans from Qt's UI thread while preserving external terminal discovery, process isolation, extra-window rejection, exit detection, and input focus.

**Architecture:** One shared X11 watcher owns a worker thread and a separate connection. Prefer the window manager's `_NET_CLIENT_LIST`, use bounded root/frame discovery when unavailable, and react to map/property/destroy events. Keep native mouse focus/replay on the UI connection, independent of discovery.

**Tech stack:** Qt 6, C++20, XCB; no new dependencies.

**Spec:** User-approved five-point correction in this conversation, supported by the 50,000-window reproduction in `/tmp/bat-ui-lag-debug`.

## Constraints

- Work in the current `dev_qt` checkout; preserve existing IPC and terminal ownership.
- Commit the completed fix and project memory as subsequently requested; do not push, publish externally, or replace the user's running app.
- Build with the existing isolated Qt/KDE SDK; follow automatic successful-build versioning.
- Query only bounded candidate sets; bound worker processing time, pending work, and discovery depth.
- Preserve first-click selection/paste, BAT control focus, wheel focus, and modal protection.

## Review focus

- Fifty thousand unrelated unmapped windows before and after terminal startup must not stall the GUI or hide the new terminal.
- Multiple simultaneous terminal sessions must remain isolated and share one watcher.
- Windows mapped before PID properties are set, WM reparenting, and no-WM Xvfb must be handled.
- Splash windows, normal terminal exit, extra windows, failed startup and app teardown must remain safe.
- Discovery must not delay synchronous mouse-grab replay.

## Task 1: Regression test

Files: `qt-ui/tests/core_test.cpp`, `qt-ui/tests/x11input.h`.
- [x] Add a native X11 fixture that creates 50,000 unrelated unmapped windows.
- [x] Verify terminal startup, GUI heartbeat and extra-window detection while those windows exist.
- [x] Build tests and observe the responsiveness assertion fail on the original implementation.

## Task 2: Shared window watcher

Files: new `qt-ui/src/x11windowwatcher.{h,cpp}`, `qt-ui/src/externalterminal.{h,cpp}`, `qt-ui/CMakeLists.txt`.
- [x] Add a shared watcher with token-based process registration, queued GUI notifications and deterministic worker teardown.
- [x] Discover candidates from `_NET_CLIENT_LIST` or bounded root/frame traversal; process X11 replies/events in bounded batches.
- [x] Preserve PID/process-group and normal-window checks, extra-window rejection and destruction notifications.
- [x] Integrate the watcher into external panels, leaving the startup timeout and native input handling independent.
- [x] Run the new regression and existing terminal tests; cover two concurrent sessions.

## Task 3: Verify and deliver

Files: `qt-ui/README.md`, generated graph/build metadata.
- [x] Document event-driven discovery and the responsiveness test.
- [x] Run the full native suite, installed WezTerm/Ghostty integration and WezTerm mouse/clipboard test under isolated X11.
- [x] Run TypeScript checking and the standard frontend build.
- [x] Build the native executable, verify version/receipt consistency, and run Graphify update with graph integrity checks.
- [x] Obtain a fresh code review, address substantive findings, and report actual validation and remaining runtime limits.

## Validation results

- Regression failed before the fix: GUI heartbeat paused for 1,721 ms.
- Final native X11 suite: 39 passed, 0 failed, one expected non-X11-path skip; maximum busy-display heartbeat gap 26 ms including startup.
- Window-manager (xfwm4) integration: 10 passed, 0 failed; maximum whole-startup/steady heartbeat gap 101 ms.
- Offscreen checks: 5 passed, 0 failed, including the skipped unsupported-platform path.
- WezTerm/Ghostty exit and cleanup, native-child handling, late PID/type/transient readiness, concurrent-session isolation, selection/paste/focus/modal protection all passed.
- Deterministic temporary injection experiment: readiness notification before old zero-PID reply is lost with replay disabled and found with the fix. Sources/results under `/tmp/bat-ui-lag-debug/race/`; no production test hooks.
- TypeScript checking, standard frontend compilation and Qt version/packaging tests passed.
- Independent review: all substantive findings addressed; final review found no blockers.
- Native executable built as 0.1.9-dev; source fingerprint and binary receipt validated.
- New local AppImage: `release/BetterAgentTerminalQt_0.1.9-dev_amd64.AppImage`; checksum verified and isolated Konsole/Rust-host smoke test passed from `/tmp`.
- AppImage SHA-256: `7356b4097e139170426256b920574001eec5ef47a060dea1660bf52a5fe7faa9`.
- Graphify: 7,278 nodes, 18,150 edges, zero duplicate IDs/dangling endpoints/locks. Six parser warnings (including Qt macro-containing watcher/test sources) limit graph extraction; C++ builds passed.
- User subsequently authorized a local Git commit and project memory update. No push, launcher change, or restart of the user's running app; existing-session acceptance awaits running the new AppImage.
