# Generic child startup and primary-thread lifecycle

`CREATE_SUSPENDED` children use a private CLOEXEC Unix socket, never SIGSTOP.
The child survives loader re-exec, captures/unsets its descriptor environment
after that re-exec, and acknowledges readiness before guest initialization
callbacks. CreateProcess does not publish handles before that acknowledgment.
The child accepts exactly one release byte; missing parents or invalid messages
fail startup rather than executing guest code. Normal children are released by
CreateProcess itself. No game, Steam identity, entitlement or wrapper routing
is involved.

Primary-thread handles reserve namespace 0x39000000 (local threads use 0x33;
processes/snapshots 0x35/36, window stations 0x37, event logs 0x38). Numeric
primary handles are not recycled. Initially suspended threads have count one;
additional SuspendThread calls before release increment it up to 127.
ResumeThread returns the previous count and releases only on one-to-zero.
Single/multiple waits observe completion, not startup or resume. ExitThread
completion is reported independently of process exit, with its full DWORD code.
Process termination and pre-start primary termination retain requested DWORD
codes. A detached reaper owns waitpid independently of closing either handle.
Closing handles does not resume or terminate the child.

## Deliberate limits

- Running primary-thread SuspendThread/TerminateThread fail with error 50;
  killing the host process would incorrectly terminate other guest threads.
- Remote memory and thread operations are served over the same socket
  (platform/win32_remote.inc): see "Remote operations" below. Remote thread
  contexts (Get/SetThreadContext) are not provided.
- Primary-thread handle duplication/inheritance is unsupported (existing
  generic duplication rejects this new handle namespace).
- Suspended in-process whoami/hostname emulation fails with error 50 rather
  than running early. Their existing non-suspended behavior is unchanged.
- Custom guest environment blocks fail with error 50; they are not silently
  ignored or printed. Existing standard handle/directory handling is unchanged.
- Startup acknowledgment has a 30-second deadline. For PE64 it is before
  import binding (which can invoke dependencies); for PE32 it is before TLS.
  Later import/TLS/entry failures remain real child failures, not CreateProcess
  initialization-success guarantees.
- At most 128 simultaneously retained child objects; the serial namespace
  eventually exhausts rather than aliasing stale handles.

## Independent verification

`make test-child-startup`: host subprocess fixture, no PE/game dependencies;
tests no first-callback marker before resume, nested counts, normal startup,
primary/process exit separation, full termination codes, closed/stale handles,
startup failure, and private descriptor non-inheritance by a host exec.

`make test-child-loader`: includes actual Win32 wrappers and spawns its own
host fixture using the command-child path; exercises CreateProcess, single and
multiple waits, ResumeThread, GetExitCodeThread/Process, close and pre-start
termination. This is not a real PE64/PE32 executable startup integration test.
## Remote operations (platform/win32_remote.inc)

After the acknowledgment the parent's per-child listener thread owns every
read on the socket ('T' primary exit, 'r' request replies, 'D' remote-thread
completion); the child runs a server thread for requests. A handle from
OpenProcess(child pid) reaches the same child as the CreateProcess handle.

- VirtualAllocEx / VirtualProtectEx / VirtualFreeEx / VirtualQueryEx run in
  the child. Read/WriteProcessMemory use process_vm_readv/writev directly (the
  caller is the child's parent).
- CreateRemoteThread[Ex]: export stubs are laid out per process, so the parent
  sends the export's module and name and the child resolves it itself. A raw
  start is accepted only inside memory the parent allocated there with
  VirtualAllocEx; anything else fails with ERROR_INVALID_PARAMETER. Remote
  CREATE_SUSPENDED threads fail with error 50.
- Suspended child: a remote thread releases the first gate (before imports),
  so the child initializes like a Windows process whose first thread starts.
  Queued remote threads run at a second gate right before the executable's TLS
  callbacks and entry. That gate waits up to 10 s for them, then for
  ResumeThread.
- The parent's handle for a remote thread is a manual-reset event, signalled
  when the child reports completion; GetExitCodeThread reports its code.
- 32-bit (WoW32) children have no remote-thread hooks: a queued thread ends
  with STATUS_ENTRYPOINT_NOT_FOUND, and the gate still waits for ResumeThread.
