---
name: psu-reverse-engineering
description: Finds, verifies and documents functions, globals and structs in the Phantasy Star Universe client (Clementine PSUC.exe / Illuminus 14.30) using Ghidra through GhidraMCP, then turns findings into PSUEx Detours hooks. Use when locating a game function or address, decompiling PSU code, verifying addresses against Clementine, reverse engineering the renderer, camera, player or input, or when the user mentions Ghidra, disassembly or offsets.
---

# PSU Reverse Engineering

## Local paths

- Game folder: `D:\Games\PSUClementine_22072025`
- Unpacked client: `D:\Games\PSUClementine_22072025\PSUC_Uncompressed.exe`
- Toolchain root: `D:\PSUExTools` (kept off C:)
- Ghidra install: `D:\PSUExTools\ghidra_12.1.4_PUBLIC` (`GHIDRA_INSTALL_DIR`)
- Ghidra project: `D:\PSUExTools\ghidra-projects\PSUClementine`
- uv cache: `D:\PSUExTools\uv-cache` (`UV_CACHE_DIR`)

## Setup (one time)

1. Ensure `PSUC_Uncompressed.exe` exists in the game folder. This Clementine build is **not** UPX-packed; the RE binary is a copy of `PSUC.exe`. Older packs used `upx -d`.
2. Import into Ghidra project `D:\PSUExTools\ghidra-projects\PSUClementine` (already done via `analyzeHeadless` if present). Language: `x86:LE:32:default`.
3. Open that project in Ghidra GUI (`D:\PSUExTools\ghidra_12.1.4_PUBLIC\ghidraRun.bat`), enable **GhidraMCPPlugin** (File → Configure → Developer), leave the program open. Claude Code MCP config is in `.mcp.json`; approve the project **ghidra** server when prompted (check with `/mcp`).

If the GhidraMCP tools aren't available, stop and ask the user to start Ghidra and the plugin. Don't guess addresses.

## Workflow

```
- [ ] 1. Start from a known anchor in docs/ADDRESS_MAP.md
- [ ] 2. Find the target with cross-references, strings or D3D/DInput imports
- [ ] 3. Decompile and identify the signature and calling convention
- [ ] 4. Rename and comment it in Ghidra
- [ ] 5. Record it in docs/ADDRESS_MAP.md
- [ ] 6. Hook it (see .claude/rules/psuex-hooks.md) and log to confirm at runtime
```

**1. Anchors.** Begin from addresses already mapped (WinMain, D3D init, camera update, view matrix) rather than searching blind.

**2. Finding targets.**
- Renderer: find the cross-references of the device vtable calls (`Present` is vtable index 17, `EndScene` 42, `SetRenderTarget` 37, `SetTransform` 44) from the global device pointer.
- Input: find the `DirectInput8Create` import, its caller (`0x00774F90`), and where `CreateDevice` and `GetDeviceState` results are stored.
- Structs: find functions that read a known global, such as the camera task or `m_playerDataPtr`, and infer field offsets from their access patterns.
- Strings: the client has debug and task names (e.g. `TCameraTaskPlay`). Search strings and follow their references.

**3. Signature.** `ECX` used as `this` means `__thiscall`. Callee cleanup (`ret N`) means `__stdcall`/`__thiscall`, and caller cleanup means `__cdecl`. Write the typedef to match exactly. A wrong convention corrupts the stack.

**5. Address map.** Add a row to `docs/ADDRESS_MAP.md`: address, name, kind (function/global/struct field), signature or type, how it was found, and the Clementine verification status. Mark it `verified` only after a runtime log or breakpoint confirmed it.

**6. Runtime confirmation.** In the Debug build, `printf` the hook hit with its arguments for a frame or two. Ask the user to run the game and paste the console output.

## Rules

- Treat decompiler output as a hypothesis until a runtime log confirms it.
- Prefer hooking a function over patching bytes. When a byte patch is required, record the original bytes in the address map.
- Write down every finding in the address map, including dead ends that are worth remembering.
