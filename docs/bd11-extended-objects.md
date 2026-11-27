# BD11 extended rasterizer / SRV prerequisites

These are internal, generic Win64 (`B11_MS`) creation entry points, **not**
advertised device capabilities. `b11_create_rasterizer_state1` is ready for a
future Device1 slot 46 and `b11_create_shader_resource_view1` for a future
Device3 slot 57. Neither is installed: device stays 43 slots, context 115;
Device1/2/3 and Context1/2/3 QI remain refused. No loader or BD12 edits.

## Independently verified ABI

Before implementation the mingw-w64 master headers were downloaded to
/tmp/beer-bd11-headers and inspected directly (2026-11-28):

- [d3d11_1.h](https://raw.githubusercontent.com/mingw-w64/mingw-w64/master/mingw-w64-headers/include/d3d11_1.h):
  RasterizerState1 IID `1217d7a6-5039-418c-b042-9cbe256afd6e`;
  DESC1 44 bytes, ForcedSampleCount DWORD10 / offset40.
  Complete 9-slot table: IUnknown0..2, DeviceChild3..6, GetDesc7 (40 bytes),
  GetDesc1 at8 (44 bytes).
- [d3d11_3.h](https://raw.githubusercontent.com/mingw-w64/mingw-w64/master/mingw-w64-headers/include/d3d11_3.h):
  ShaderResourceView1 IID `91308b87-9040-411d-8c67-c39253ce3802`;
  DESC1 28 bytes, legacy DESC24. PlaneSlice is inside the active union:
  Texture2D DWORD4 / offset16, Texture2DArray DWORD6 / offset24.
  Complete **10**, not 9, slots: IUnknown0..2, DeviceChild3..6,
  View GetResource7, SRV GetDesc8 (24 bytes), GetDesc1 at9 (28 bytes).
- d3d11.h verifies DeviceChild `1841e5c8-16b0-489b-bcc8-44cfb0d5deae`,
  Resource `dc8e63f3-d12b-4952-b47b-5e45026a862d`, RasterizerState
  `9bb4ab81-ab1a-4d8f-b506-fc04200b6ee7`, and SRV
  `b0e06fe0-8192-4e1a-b1ca-36d7414710b2`.

Layout sizes/offsets have compile-time assertions. Extended QI requires the
correct kind **and** dedicated complete vtable, not merely spare storage.
Base states/SRVs do not expose extensions; RTV/DSV/UAV keep their old table.
Extended children retain the device; SRV retains its resource. GetDevice and
GetResource return independently retained references with normal IUnknown identity.
Private-data methods now use the shared owned D3D11 store, including real
interface references and copied bytes; see [bd11-private-data.md](bd11-private-data.md).
Complete slot layout is still not a claim of full child-interface semantic
conformance outside the documented implemented contracts.

## Rasterizer1

Fill2/3, Cull1..3, BOOL0/1, and finite bias floats are validated before any
allocation. Signed DepthBias and finite positive/negative bias values are
preserved. Valid ForcedSampleCount values are 0,1,2,4,8,16; nonzero returns
E_NOTIMPL before allocation. Other counts return E_INVALIDARG. Force0 has
exactly the legacy40-byte pipeline hash and backend state.

The existing backend never forwards legacy MultisampleEnable or
AntialiasedLineEnable to its PSO. The new path explicitly rejects either true
with E_NOTIMPL, rather than implying line-AA/quad-line or lower-feature MSAA
semantics. The broad legacy rasterizer creation policy is unchanged.
This is not forced-sample rendering, RasterizerState2, or a state-object
deduplication/4096-object-limit implementation.

NULL descriptor is invalid. NULL output performs allocation-free validation
and returns S_FALSE for supported inputs. Microsoft Learn marks the raster
output optional but **does not explicitly specify S_FALSE** on that page;
S_FALSE here is the internal prerequisite's validation-only convention,
not a claim that this return value was explicitly documented for Rasterizer1.

## SRV1

Known BD11 resource vtables/kinds, same-device ownership, SRV bind, live backing,
matching dimensions/sample mode, cube requirements, typed-compatible formats,
and nonempty mip/layer/buffer ranges are validated before construction.
Range checks use subtraction/division, not overflowing additions/products;
UINT_MAX texture ranges normalize to the remaining extent. Zero or invalid
ranges are rejected instead of inheriting legacy clamping. Buffer validation
uses logical ByteWidth, not its padded backing allocation.

Supported: typed single-color-plane buffers; 1D/array, 2D/array plane0,
2DMS/array, 3D, cube/array textures for the conservative format-family table.
Actual creation reuses the existing BD11 `create_view` and BD12 mapped image
or typed buffer descriptor writers. There is no fake GPU descriptor success.
Default descriptors are normalized to actual format/dimension/full ranges.
Provided descriptors preserve inactive union bytes; GetDesc1 roundtrips the
normalized28 bytes, GetDesc copies24 with the 2D plane field stripped.
NULL output returns documented S_FALSE after successful validation without
calling a descriptor writer or allocating a heap/object.

Nonzero 2D planes, planar/video formats, depth/stencil formats, and images whose
actual Vulkan aspect is not exactly COLOR return E_NOTIMPL. No inferred/faked
depth/stencil plane0/1 support. Raw/structured buffers are validated but return
E_NOTIMPL; the existing backend untyped texel fallback is not advertised as an
extended structured/raw contract. A typed buffer with ByteWidth too large for
legacy padded-size arithmetic also returns E_NOTIMPL. Unsupported requests
never construct a view or call the backend. Allocation and descriptor-write
failures clear output and unwind resource refs; failed heaps retire through
the existing grave queue.

Legacy SRV creation remains on device slot7. Its GetDesc overwrite was corrected
from internal32 bytes to ABI24; descriptor input is still read as legacy24.
No legacy range/format policy was tightened.

## CPU contract tests

`make test-d3d11-extended-objects` builds all fixture-linked sources at O2.
`BD11_EXT_TEST_EXTRA_CFLAGS=-O0` repeats all-source O0;
`BD11_EXT_TEST_EXTRA_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer --param=asan-globals=0'`
adds sanitizer/leak checking (the last option permits unused BD12 COM globals
to be garbage-collected in this standalone fixture).

The fixture invokes actual typed Win64 creation entry points and actual object
slots. Only resource allocation / descriptor writing / retirement fences are
injected; it checks writer arguments and real base slot7 SRV regression,
QI/identity/GetDevice/GetResource lifetime, defaults, format families,
dimensions including MS/cube, overflow/range/type/bind/foreign-device failures,
unsupported plane/aspect/force requests with no allocation/backend calls,
and object/heap/writer failure rollback. Input descriptors sit at inaccessible
page boundaries at exactly40/44/24/28 bytes; output has canaries and guard pages.

This is CPU ABI/ownership/normalization validation, **not GPU rendering or
application success**. No guest launch is needed.

Verified results (2026-11-28):

| Check | Result |
| --- | --- |
| New fixture, all-source O2 | PASS, exit0; 16 image writer calls / 1 buffer writer call |
| New fixture, all-source O0 | PASS, exit0; same counts |
| New fixture, ASan+UBSan with leak checking | PASS, exit0; same counts, no sanitizer report |
| Full `make -B -j8` | PASS, exit0 |
| `test-d3d11-context-state` | PASS, exit0 |
| `test-d3d11-multithread` (including device/context version QI negatives) | PASS, exit0 |
| `test-d3d11-creation` | PASS, exit0 |
| Full `test-input` | PASS, exit0 |
| Edited-file diagnostics / scoped diff whitespace check | PASS |

The fixture's one logged view-creation failure is deliberately injected to
exercise descriptor-writer rollback. Existing build warnings remain; no new
warnings were reported in the added code. Concurrent edits elsewhere in the
workspace, including loader/BD12, were preserved; this task did not modify
those sources. No guest launches or commits.

Contract references:
[CreateRasterizerState1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-createrasterizerstate1),
[raster fields and force counts](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/ns-d3d11_1-d3d11_rasterizer_desc1),
[CreateShaderResourceView1 including NULL-output S_FALSE](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11device3-createshaderresourceview1),
[SRV1 format compatibility](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/ns-d3d11_3-d3d11_shader_resource_view_desc1).