# PSUEx

Path-scoped rules for hooks and VR work live in `.claude/rules/`. The Ghidra reverse-engineering workflow is the `psu-reverse-engineering` skill.

## Building PSUEx

- The solution is `PSUEx.sln` (MSVC v143, C++latest). Only `Win32` builds load into the game; ignore the `x64` configurations.
- Requires the DirectX SDK (June 2010), found through `$(DXSDK_DIR)`.
- Debug outputs `PSUExD.dll` and opens a console for `printf` logging. Release outputs `PSUExVR.dll`. The game folder's `dinput8.dll` loader loads `PSUExD.dll` first, then `PSUExVR.dll`.
- After C++ changes, compile before reporting done:

```powershell
msbuild PSUEx.sln /p:Configuration=Debug /p:Platform=x86 /m /v:minimal
```

If `msbuild` isn't on PATH, find it with `& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find MSBuild\**\Bin\MSBuild.exe`.

- New `.cpp`/`.h` files must be added to `PSUEx.vcxproj` (and `PSUEx.vcxproj.filters`), or they won't compile.
- In-game testing is manual. Ask the user to copy the DLL into the game folder and report the console output.

## Sessions and token usage

- `docs/VR_HANDOFF.md` is the hand-over between sessions. Read it at the start of VR work. Update it before ending a stretch of work or switching topics: what's in flight, game-folder state (settings flags, what's deployed), pending tests and their expected results.
- One task per session. When the user moves to an unrelated topic, or the `UserPromptSubmit` size reminder fires (`.claude/hooks/session-size.js`, from ~150k context tokens), update the handoff doc and suggest a new session or `/clear` in one line instead of carrying the old context on.
- Read the game log with `tools/lastlog.sh [-d] [PATTERN]` (last attach only, deduped), never by tailing or reading the 100+ MB `psuex.log`.
- Compare game settings files against `.bakN` backups with `diff --strip-trailing-cr`; the line endings differ.
