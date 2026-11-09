---
paths:
  - "PSUDetour/**"
  - "PSUMemory/**"
  - "PSUVR/**"
  - "dllmain.cpp"
---

# PSUEx Hooks and Patches

The target is the 32-bit PSU Illuminus 14.30 client. All game addresses are absolute and fixed (no ASLR). Build only the `Win32` configurations.

## Adding a hook

- One hook per file: `PSUDetour/Hook_<address or TypeName>.hpp`, wrapped in a namespace with a `Hook()` function.
- Register `Hook()` inside `DetoursHookAll()` in `dllmain.cpp`, with a short trailing comment.
- Game member functions are `__thiscall`. Hook them as `__fastcall` with a dummy `edx` argument:

```cpp
namespace sub_48DA90
{
	typedef int( __thiscall *function_t )( void *thisPtr );
	function_t pOriginal = reinterpret_cast< function_t >( 0x0048DA90 );

	int __fastcall Hook_48DA90( void *thisPtr, void * /*edx*/ )
	{
		return pOriginal( thisPtr );
	}

	void Hook()
	{
		DetourAttach( &( PVOID & )pOriginal, reinterpret_cast< PVOID >( Hook_48DA90 ) );
	}
}
```

## Addresses

- Every new address goes into `docs/ADDRESS_MAP.md` with what it is, how it was found, and whether it was verified on the Clementine `PSUC.exe`.
- Read and write game memory through `PSUMemory` (`ReadAbsoluteMemory`, `WriteAbsoluteMemory`), which checks pointers first. Don't dereference raw game pointers that may be null.
- Never hook or rewrite network send or receive paths for VR features.
