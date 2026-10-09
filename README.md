# HippaulMod

Formerly CrystalMod. Existing installs keep updating on their own; nothing needs reinstalling.

A small in-process add-on for Square Enix's PlayOnline Viewer that lets an
unmodified client talk to a private server (such as OpenLobby) and run its
titles on modern Windows and on Steam Deck / Proton. It does two things:

- **Routing**: resolves the client's `*.pol.com` / `*.playonline.com` /
  `*.sqex.net` names to the server address you configure, in-process, with
  no hosts file or DNS change. Strict mode refuses any unmapped name so
  nothing leaks to the internet.
- **Game fixes**: the compatibility work the 2003-era client and its titles
  need today: windowed and scaled D3D8/D3D9/DirectDraw, DirectInput mouse
  and gamepad seams, focus and sleep/resume recovery, registry repair for
  copied installs, per-title patches applied in memory after unpack, a
  Proton D3D8-on-DXVK opt-in, OS dialog translation, and an in-game
  settings dialog (Home key, or Back+Start on a pad).

Nothing else: no telemetry, no automation, no bundled third-party tools. The
one thing that sends data is a problem report you file yourself (see
"Reporting a problem").

## How it works

`pol.exe` statically imports `PolHook.dll`. The installer renames the
game's own `PolHook.dll` to `PolHook_orig.dll` and puts ours in its place;
ours forwards the six original exports to `PolHook_orig.dll` and runs its
hooks from `DllMain`, before the Viewer's entry point. Square Enix's file
is renamed, never modified or redistributed. Uninstall = rename it back.

## Download

Every release on the repository's Releases page carries `PolShimSetup.exe`
(the Windows installer), `PolHook.dll` with its `PolHook.dll.sha256`,
`install.sh` and `Install-PolHookProxy.ps1`, the `polshim.ini` template, a
`SHA256SUMS` file and a zip of the whole set. The Windows XP build sits beside
it as `PolShimSetup-xp.exe` and `PolHook-xp.dll` (see below). Every push to `main` also
leaves the same set as a workflow artifact for anyone who wants the newest
build before a release is cut.

## Install

**Windows**: run `PolShimSetup.exe`. It asks which server to use and offers
`play.openlobby.fyi`; press Enter for that, or type another name or IP address. Command line:
`PolShimSetup.exe --server=ADDR`.

**Steam Deck / Proton**: `bash install.sh` from the `dist/` folder; it
finds the PlayOnline install under Steam, asks for the server (same default),
and enables D3D8 through DXVK. `install.sh --revert` undoes everything.

Both installers also keep PlayOnline's own `file.txt` in step. That file is the
manifest the Viewer checks itself against, and replacing `PolHook.dll` without
rewriting its line leaves the install disagreeing with itself: PlayOnline then
either puts SE's DLL back at the next Check Files, or stops during an update and
says a file is missing. The original manifest is kept as `file.txt.polshim-orig`
and `--revert` puts it back. On Linux this step needs `python3`; without it the
installer says so and carries on.

A server name is looked up once, by the installer, and written to `polshim.ini`
as an IP address, because that is the form the shim reads. If the server moves
to a new address, run the installer again.

Settings live in `polshim.ini` next to `pol.exe`; the in-game dialog edits
the same file. `[redirect] server=` is the only required value.

## Windows XP

The normal build needs Windows Vista or later. On XP, PlayOnline will not
start at all with it installed, because `pol.exe` loads `PolHook.dll` before
anything else runs. XP players install `PolShimSetup-xp.exe` instead. It is
the same shim, built with the Visual Studio 2017 XP toolset and without SSE2
instructions, so it also runs on a Pentium III or an Athlon XP.

The XP build updates itself from `PolHook-xp.dll` in each release and never
installs the normal `PolHook.dll`. XP cannot connect to GitHub on its own (it
stops at TLS 1.0), so the XP build carries its own TLS client for that. If an
update check fails with a certificate error, check that the PC's clock is set
to the right date.

## Updates

The shim checks the project's latest GitHub release hourly and offers newer
builds; it never installs an older one. Both installers fetch from the same
place. Set `[autoupdate] enable=0` to opt out.

Updates do not come from the game server you play on. A server operator who
wants to host a build for their own players unzips a release's set into
`www/shim/dist/` of the OpenLobby checkout and has them set `[autoupdate]
url=server` (installers: `--update-url=server`, or `POLSHIM_UPDATE_URL=server`
for `install.sh`). The `PolHook.dll.sha256` in that folder is what the updater
and `install.sh` compare against.

## Reporting a problem

Press **Ctrl+Shift+R** (or View + right shoulder on a controller) while the
Viewer or a game is in front. HippaulMod takes a screenshot at that moment,
then opens a box where you describe what went wrong. Nothing is sent until
you click **Send report**; Cancel throws everything away.

A report goes to the game server you play on (never to Square Enix, and
never anywhere if no server is set up) and contains:

- your description and the kind of problem you picked;
- your computer's name, the Windows version and the PlayOnline session id
  (so the server can find the same minutes in its own logs);
- the end of this session's HippaulMod log (and the previous session's, if
  it crashed), and your `polshim.ini`, with passwords, login tokens and
  session keys blanked out first;
- a picture of the game window;
- `diag.txt`: GPU and driver version, each monitor's resolution and display
  scaling, Windows compatibility settings on `pol.exe`, which graphics DLLs
  are loaded (and whether they are Windows' own or a wrapper such as DXVK),
  programs that have loaded themselves into the game (overlays and
  recorders), the game's windows, and one second of frame counting.
  Folder paths under your user profile are shortened to `%USERPROFILE%`,
  and for other programs' windows only the program's file name is
  recorded, never the window title.

The server answers with a report id; include it if you talk to the server's
admins about the problem. Each part can be switched off under "Reporting a
problem" in the settings dialog, or in `polshim.ini` (`[report]`).

If the key does nothing, open the settings window (Ctrl+Shift+S) and use
**Report a problem now...** under "Reporting a problem". It sends the same
report.

On Linux and Steam Deck a report also carries Proton's own log and DXVK's
logs when they exist. Proton only writes its log when the game is started
with `PROTON_LOG=1 %command%` in Steam's Launch Options.

### Live log

For a problem that takes a while to show up, tick **Send my log to the
server as I play** under "Reporting a problem", when someone helping you
asks. The log then goes to the server every few seconds, with passwords
removed, and the title bar says so. It turns itself off after 30 minutes,
including across restarts.

## Build

`build.bat` with Visual Studio 2022 (x86 tools) produces `build\PolHook.dll`
and `build\PolShimSetup.exe`. The normal build uses no third-party libraries; hooks, image
patching and the PNG writer are in-tree. CI builds every push on a Windows
runner and publishes both artifacts.

`build.bat xp` makes the Windows XP build in `build-xp\`. It needs two more
components from the Visual Studio installer: "MSVC v141 - VS 2017 C++ x64/x86
build tools" and "C++ Windows XP Support for VS 2017 (v141) tools". The XP
build adds BearSSL (`third_party/bearssl`) for HTTPS. `python xpcheck.py
build-xp\PolHook.dll` confirms a build imports nothing XP lacks; CI runs it on
every push.

## What is not included

- No Square Enix files. The shim is a proxy for a file that stays on your
  disk under its new name.
- No server of its own. The installers offer the OpenLobby public server and
  take any other address you give them.

## License

AGPL-3.0 (see LICENSE). `third_party/bearssl` is BearSSL 0.6 by Thomas Pornin,
under the MIT licence in `third_party/bearssl/LICENSE.txt`.
