# BD11 D3D11 private data

Generic GUID16-keyed storage for existing D3D11 device slots34..36 and
DeviceChild slots4..6 (resources, views, shaders, layouts, states, queries,
contexts, command lists, and extended rasterizer/SRV children). All three
entry points have complete typed Win64 `B11_MS` signatures. No new interfaces,
version QI, device capabilities, application-specific keys, or debug-name
special cases. No loader, virtual-memory, or BD12 source edits.

## Contract and references

- [Microsoft Learn SetPrivateData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicechild-setprivatedata):
  copies the supplied bytes; NULL data requires size0 and removes the old key.
  NULL/nonzero is rejected with E_INVALIDARG, leaving the old entry unchanged.
- [GetPrivateData](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicechild-getprivatedata)
  and [SetPrivateDataInterface](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicechild-setprivatedatainterface):
  the store owns one IUnknown reference; successful interface retrieval owns a
  new caller reference; replacement or destruction releases the store reference.
- [Wine D3D11 conformance test `test_private_data`](https://github.com/wine-mirror/wine/blob/master/dlls/d3d11/tests/d3d11.c)
  verifies missing key DXGI_ERROR_NOT_FOUND with output size0; too-small buffer
  DXGI_ERROR_MORE_DATA with required size and no buffer writes; NULL size
  E_INVALIDARG; size-only queries S_OK with no AddRef; absent blob deletion
  S_FALSE and existing deletion S_OK; NULL interface stores a pointer-sized
  NULL value (S_OK), **not an absent key**. The old interface reference is released.
- [Wine `d3d_set_private_data` / `d3d_set_private_data_interface`](https://github.com/wine-mirror/wine/blob/master/dlls/d3d11/utils.c)
  distinguishes NULL blob deletion from a non-NULL zero-size blob (an existing
  empty value). BD11 preserves that distinction. Native conformance tests also
  accept NULL/nonzero blob deletion; BD11 deliberately follows the stricter
  Microsoft documented input precondition instead, as required for this work.

NULL owner/GUID is rejected with E_INVALIDARG. Size-only queries ignore the
input capacity; all failed gets leave the data buffer unchanged. Required
interface size is the actual Win64 pointer size (8). Pointer-sized byte blobs
are never interpreted as interfaces. Keys compare all16 bytes, regardless of
GUID alignment. No borrowed source memory or special debug GUID behavior.

## Ownership, locking and lifetime

`B11Obj` has a zero-initialized lazy list pointer and destruction tombstone;
no heap mutex or per-object initialization can fail. A stable static mutex
protects lists and tombstones. Immutable entries have atomic ownership/pin
counts. Bytes are copied and a real foreign IUnknown slot1 AddRef is made
**before** publication. Allocation failure leaves the old entry untouched.
Self replacement is safe. Removed entries are dropped outside the mutex;
their real foreign slot2 Release is never a `B11Obj` refcount assumption.

A successful interface getter pins its entry under the mutex, unlocks, calls
AddRef, copies the pointer, then unpins. Concurrent deletion/replacement cannot
release the store reference while AddRef is executing. Queries/undersized gets
never enter foreign code. Reentrant callbacks can read/replace the same key.
Context private methods deliberately do not acquire the backend context guard:
the store is independently synchronized, and foreign code must not inherit
that lock from these methods. An explicit caller-held Multithread Enter lock
is the caller's responsibility; these methods do not release caller locks.

Final `b11_release` detaches the list and marks the owner dead before callbacks
and before its existing destructor. Reentrant private calls during cleanup
return E_INVALIDARG, preventing reinsertion even if invalid code resurrects a
zero-ref object. Callers must hold a valid owner reference for concurrent calls.
Ordinary COM reference cycles (including storing oneself) remain caller-owned
cycles, not silently weakened references.

Lifetime audit: buffer/texture/view/layout direct failure destructors operate
only on unpublished objects; no private calls or callback exposure exists in
those construction paths. Context/command-list published objects use common
Release and retain their device through cleanup. Extended state/view children
release their retained device only after private cleanup. Device construction
rollback and swap construction rollback are likewise unpublished. Existing
production devices remain process-owned; this does not change GPU teardown.

## Deliberate DXGI exclusion

DXGI slots3..5 have **different ordering** (Set, SetInterface, Get). BD11's
current IDXGIDevice is a separate static forwarding object whose mutable
`dev` field is overwritten on query, not a safe per-device shared owner.
Swapchains have DXGI identity/signatures rather than DeviceChild slots.
Neither is wired to this store; existing DXGI stubs remain unchanged. No claim
of D3D11/DXGI shared private-data identity or full DXGI conformance is made.

## CPU validation

`make test-d3d11-private-data` invokes actual typed COM slots on a stack device,
immediate/deferred contexts, a real FinishCommandList result, and base/extended
rasterizer children. Checks include copy ownership, full16-byte key and object
isolation, empty/absent/NULL values, size/canaries, reference balancing,
interface/blob replacements, self cycles, malloc failure rollback,
multi-entry final-release cleanup for the tested objects, tombstoned reentrancy,
same-key AddRef/Release callbacks,
and a condition-variable-controlled getter pin across replacement/deletion.
Four concurrent workers mix interface gets/replacements/deletions and copied
blob gets/replacements/deletions while retaining the owner. The extended-object
fixture also exercises private-data roundtrips and self-reference release on
actual RasterizerState1 and ShaderResourceView1 tables.

`BD11_PRIVATE_TEST_EXTRA_CFLAGS=-O0` selects all-source O0.
`BD11_PRIVATE_TEST_EXTRA_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer --param=asan-globals=0'`
selects sanitizer validation; the last switch allows unused BD12 COM globals
to be GC-linked in the standalone fixture. No GPU rendering, Windows-native
execution, guest launch, or application-success evidence is implied.

Verified 2026-11-28: private-data O2/O0/ASan+UBSan (including leak checking)
PASS; extended-object O2/O0/ASan+UBSan PASS; forced `make -B -j8` loader rebuild
PASS; context-state,
multithread, creation and full input regression targets PASS. Existing unrelated
backend/compiler warnings remain; this is not a warning-free-build claim.