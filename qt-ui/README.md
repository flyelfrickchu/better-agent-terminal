# Native Qt/KDE UI

The `dev_qt` migration uses Qt 6 Widgets and KDE Frameworks 6. Every local
terminal embeds Konsole's KPart, which owns its shell and PTY. Agent requests
use the existing BAT host protocol (`bat-remote/v2`) and profile contexts.

## Build on Kubuntu

```sh
sudo apt-get install cmake qt6-base-dev qt6-websockets-dev libkf6parts-dev libkf6coreaddons-dev konsole-kpart
cmake -S qt-ui -B qt-ui/build -DCMAKE_BUILD_TYPE=Debug
cmake --build qt-ui/build -j2
ctest --test-dir qt-ui/build --output-on-failure
./qt-ui/build/better-agent-terminal-qt --server /path/to/bat-server
```

Build the headless Rust host and native UI together with `pnpm run qt:build`,
then launch with `pnpm run qt:start`. The executable also looks for a built
Rust host automatically, or accepts `BAT_QT_SERVER`. Its private local host
starts on an available loopback port. Use `--data-dir` or `BAT_QT_DATA_DIR` to
select its persistent state directory.

Use **Add workspace** to select a folder. Each local workspace starts with
a Konsole tab; **Konsole** adds another. Closing a workspace or terminal
asks for confirmation because it terminates its running processes.

Use **Connect** with the connection URL from BAT's remote server.
The URL must contain a token and SHA-256 certificate fingerprint. It is not
saved to settings. Select the host profile from the toolbar. **Claude** and
**Codex** create agent tabs with streaming conversations, native permissions,
question dialogs, and host-owned session state. Workspaces and Qt tab descriptors
are saved through the existing host API without removing legacy terminal records.
Konsole runs locally; the agent working directory must also exist on the host.
Use a host on the same machine for a shared working directory.

**Settings** opens native tabs for interface appearance, Konsole profiles,
agent model/effort defaults, workspace defaults, and Python virtual environments.
Settings are loaded from and saved to the active host. Interface appearance
applies immediately; terminal profiles and agent defaults apply to new tabs.
Terminal shell, font, and colors are managed by the selected Konsole profile.
Unknown settings and the built-in status line configuration are preserved.
Loading, connection, validation, and save failures are shown in the dialog.

**Git** switches to an embedded workspace view above the running terminal/agent
sessions. Its three columns show commits (including uncommitted changes), changed
files, and Diff/File previews. Select a commit and then a file to browse its diff;
the File tab shows the current working copy, matching the original UI. Added lines
are green, deleted lines red, hunk headers blue, and file statuses are colored.
Untracked text files are displayed as additions. **Refresh** reloads the repository;
the **Terminal** tab restores the full session view without restarting sessions.

This is an initial migration scaffold, not feature parity with the React UI.
The configurable built-in status line, complete tool rendering, advanced agent
controls, and remaining advanced settings still need migration. Native settings,
history browsing, file previews, and Git views are available. Existing React
and backend implementations are retained during migration.

Git commit messages, file paths, diffs, and file contents wrap to the available column width, including long strings without spaces. Use the Git tab’s × button to close the view and return to Terminal; the Git toolbar button reopens it without interrupting sessions.
