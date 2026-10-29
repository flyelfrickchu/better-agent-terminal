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

## Automatic Qt versions and AppImage names

`qt-ui/version.json` records the last successful native build. CMake compares
the content of Qt/runtime source and build inputs; changed inputs increment the
version once when the native targets compile successfully. Identical rebuilds,
documentation edits and test-only changes keep the same version. Failed builds
do not commit the proposed version. Keep this file with source changes when
committing so subsequent checkouts continue the sequence.

Patch and minor digits range from 0 to 9: `0.0.9` becomes `0.1.0`, `0.9.9`
becomes `1.0.0`, and `9.9.9` becomes `10.0.0`. The executable's `--version`
and AppImage filename use the same compiled version with a `-dev` suffix.

After building, package against an existing prepared Qt/KDE AppDir and an
existing Qt AppImage (used only for its type-2 loader):

```sh
pnpm run qt:build
pnpm run qt:package:appimage
```

For the isolated SDK build directory used on Ubuntu 24.04:

```sh
cmake --build dist/qt-build/native -j2
pnpm run qt:package:appimage --build-dir dist/qt-build/native
```

The packaging command updates the Qt executable in a separate copy of the
prepared `release/BetterAgentTerminalQt.AppDir`, retaining its bundled host,
sidecar and Qt/KDE libraries. Stage those runtimes again when they change.
Use `--appdir PATH` and `--runtime-from IMAGE` to select other prepared inputs.
The command verifies the compiled version and current source fingerprint, then
writes `release/BetterAgentTerminalQt_<version>-dev_amd64.AppImage` and a SHA-256
file. It refuses stale builds and existing output names. For example, the first
build after `0.1.0` produces `BetterAgentTerminalQt_0.1.1-dev_amd64.AppImage`.

`pnpm run test:qt-version` checks version carry, source changes and packaging
consistency without building the full application.

The **+** beside **Workspaces** creates a Project group. Enter a custom name
or keep the first unused default name from `Project_00` through `Project_99`.
The **+ / −** on the left of a Project expands or collapses its workspaces;
the **+** on the right adds a workspace inside that Project. Right-click a
Project to rename it or choose **Remove project**. Removal asks for confirmation,
then removes the Project and all its workspaces, closes their Qt tabs, and stops
their agent sessions. Files on disk remain. Empty Projects can also be removed.
Groups, empty Projects, membership, and collapsed state
are saved with the host profile. Existing workspaces appear in `Project_00`.

Use **Add workspace** to select a folder in the selected Project. Each local workspace starts with
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
