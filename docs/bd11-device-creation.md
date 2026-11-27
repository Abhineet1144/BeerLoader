# BD11 device creation configuration

The default BD11 loader path caches process-owned devices by the exact creation
flags and the feature level granted by the existing loader policy. The constructor
sets both fields before creating the immediate context or publishing the device.
Repeating a key reuses that device/context; a different key creates separate BD11
state. Caller references are independent of the process-owned references.

Accepted flags are **0**, **SINGLETHREADED (0x1)**, **BGRA_SUPPORT (0x20)**, and
their combination **0x21**. SINGLETHREADED is a caller usage contract, not a new
thread-safety promise or an instruction to disable internal backend locks. BGRA
uses the existing backend B8G8R8A8 UNORM/SRGB format mappings; this change records
the request for GetCreationFlags, not new Direct2D functionality.

DEBUG (0x2), including combinations, returns
DXGI_ERROR_SDK_COMPONENT_MISSING (0x887a002d): BD11 has no D3D11 debug layer.
Every other flag bit returns E_INVALIDARG (0x80070057), before construction.
This deliberately tightens previously ignored flags. Callers using unsupported
debuggable, video, protection, or optimization flags may now fail creation rather
than continue with an implementation that did not honor the request. Unknown
flags are not silently masked and no unimplemented capability is reported.

Driver type, adapter selection, Software, SDK-version validation, and the granted
feature-level policy are unchanged (including the explicitly untruthful existing
BEER_FAKE_CAPS testing switch). Distinct cache entries do **not** establish
independent software/WARP/reference drivers or GPUs. The legacy compatibility
path is unchanged and does not gain this configuration contract. Device3 and
Context3 remain unexposed.

For CreateDevice, only requested COM outputs receive references. For
CreateDeviceAndSwapChain, the temporary device reference is transferred or
released, the optional context reference is transferred on success, and both
are released on swap-chain failure. Outputs remain null on failure and the
feature-level output is written only on success. The existing requirement for
a swap descriptor and swap output in AndSwapChain is retained; an invalid call
on the BD11 path no longer falls through to the legacy objects.

`make test-d3d11-creation` runs CPU-only tests of the actual cache helper with a
local constructor and the actual included-loader Win64 wrappers with linker-only
injection. Tests cover cache keys/reuse, concurrent first publication, constructor
failure/retry, all accepted/rejected flags, real COM GetCreationFlags/GetFeatureLevel,
immutable earlier publication, optional outputs, swap failure rollback, and refusal
of Device3/Context3. A wrapped GPU constructor aborts if reached. Swap construction
is a CPU lifetime fixture, not proof of Vulkan presentation or Direct2D rendering.
Devices/backend allocations remain process-owned for each distinct configuration;
more configurations can therefore consume more memory than the old singleton.