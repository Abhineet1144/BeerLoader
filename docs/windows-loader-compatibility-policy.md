# Windows Loader Compatibility Policy

## Status

This policy is mandatory for Beer loader work. It applies to executable loading, DLL search, native file and section APIs, compatibility modules, protected virtual filesystems, and Steam integration.

## Objective

Beer reproduces general Windows behavior. It must not make an executable work by recognizing that executable and changing control flow or runtime results specifically for it.

A compatibility change is acceptable only when it describes a reusable Windows contract that can be tested independently of the title that exposed the defect.

## Allowed work

Beer may:

- Implement documented Win32, NT, PE/COFF, TEB, PEB, TLS, loader-list, exception, memory, file, section, registry, graphics, input, and networking contracts.
- Use Windows behavior observed on a lawful reference installation as an external oracle.
- Compare API ordering, return values, structure layouts, module topology, and error behavior against that oracle.
- Execute application-shipped guest PE modules through Beer's normal PE loader.
- Honor guest-installed native API hooks when Windows would route the same operation through those hooks.
- Add diagnostics that identify modules, APIs, structures, and generic state without changing guest execution.
- Add explicit user-selected runtime policies such as `facade`, `guest`, or `official`, provided their behavior is module-contract based and documented.

Reference environments must remain external oracles. Their binaries, source, private export inventories, fixed addresses, and implementation constants must not become Beer dependencies.

## Prohibited work

Beer must not:

- Match an executable name, path, hash, app ID, RVA, byte signature, protection brand, or title-specific asset to select a hidden compatibility patch.
- Patch guest instructions, skip guest checks, force guest branches, or synthesize successful return values solely to bypass a title's failure path.
- Forge ownership tickets, licenses, authentication tokens, depot keys, signatures, or authenticated sessions.
- Replace a failed entitlement or integrity decision with success.
- Copy or load Wine, Proton, Windows, Steam-client, or other foreign runtime files into Beer. The approved shader translator remains the existing exception.
- Expose writable aliases or symlinks to user-owned external runtime files.
- Advertise an API as successful without initializing every required output according to its contract.
- Invent GPU, CPU, network, or device capabilities that the host cannot provide.

## Native loader ordering

The generic module-loading model is:

1. Normalize the requested module name according to Windows rules.
2. Return an already loaded module identity when Windows would do so, including process-owned runtime modules and KnownDLL-style modules.
3. Apply normal API-set and system-module ownership rules.
4. For unresolved application-private modules, perform the native file/section path. If the process has installed guest `Nt*`/`Zw*` file or section hooks, those hooks observe the operation in the same place they would on Windows.
5. If the hooked native layer declines the operation, continue normal application-directory and configured DLL-directory search.
6. Use a Beer compatibility facade only according to an explicit, generic fallback policy.
7. A successfully loaded guest image receives normal relocation, import binding, static TLS, process-attach notification, section protection, module identity, path identity, and `PEB->Ldr` publication.

System module identity must remain stable. An application-private virtual filesystem must not create a second `kernel32.dll`, `ntdll.dll`, or other process-owned runtime module.

## Native file and section consistency

Kernel32 file and mapping operations must remain coherent with the native layer:

- `CreateFile*`, file attributes/information queries, section creation, image mapping, and `LoadLibrary*` must agree on which object a Windows path identifies.
- A virtual file supplied by a guest hook must remain authoritative for subsequent section and image operations.
- Hook recursion must be detected and handled without bypassing the hook on the original operation.
- `SEC_IMAGE` mappings use PE image layout, not flat file layout.
- File access masks must follow NT semantics; synchronization rights alone do not imply write access.
- Guest-visible module paths use normalized DOS paths, not host filesystem paths.

## Steam boundary

Steam behavior follows the selected generic runtime mode:

- `facade`: Beer-owned offline interfaces only.
- `guest`: application-shipped guest modules run through the normal Windows module path.
- `official`: an external official Steam process handles authentication and ownership; Beer does not fabricate its state.

The loader must not silently execute a host Steam DLL. A local guest module may be selected because of normal Windows search/VFS behavior, not because Beer recognizes a game.

## Architecture portability

Beer targets ARM hosts as well as x86-64 hosts. Windows compatibility policy and loader state must therefore remain host-architecture neutral wherever possible.

- Core loader, file, section, module, PE, and policy logic must be portable C and must not depend on host x86 instructions or register layout.
- Guest x86-64 ABI handling belongs behind an explicit CPU/ABI backend boundary so an ARM build can use its translation backend without duplicating Windows policy.
- New x86-specific assembly, raw host register access, syscall interception, or instruction emulation requires explicit user confirmation before implementation.
- Focused policy tests must run without executing guest x86 code where practical.
- Existing x86-specific code may be called through its established abstraction, but generic fixes must not deepen that dependency.

## Evidence required for a runtime change

Every retained runtime change should include:

1. The generic Windows contract being corrected.
2. Evidence showing the previous behavior violated that contract.
3. A focused test independent of the reporting executable where practical.
4. A bounded launch test for the reporting workload.
5. Regression gates for previously working workloads when hardware permits.
6. A progress-file entry recording the result, including neutral or failed experiments.

A change that only moves a title farther but cannot be stated as a reusable Windows rule is not sufficient for retention.

## RE4 acceptance boundary

RE4 is a useful regression workload, not a policy selector. A valid RE4 fix must demonstrate all of the following:

- No RE4 executable name, app ID, hash, RVA, byte pattern, RUNE identifier, or package-specific filename pair controls loader behavior.
- The same native file/section ordering applies to an independently constructed test PE and virtual-file hook.
- Already loaded system modules keep their original identities.
- An application-private virtual image can be opened, section-mapped, loaded, initialized, and published through generic APIs.
- The loader does not forge the Valve ownership ticket or modify the verification path.
- RE8 and other facade-mode titles retain their established module policy.

The desired outcome is that the package's own Windows-visible module arrangement is selected naturally by generic loader semantics. Beer does not recreate or alter the package's bypass logic.

## Review checklist

Before retaining a change, ask:

- Would this code behave identically for another executable making the same API calls?
- Can the rule be described without naming the reporting title?
- Is every success result backed by truthful state and initialized outputs?
- Does the change preserve system-module and loader-list identity?
- Does it avoid foreign runtime dependencies and entitlement fabrication?
- Is there a focused test or a clear plan to add one?

If any answer is no, the change must be redesigned or rejected.
