#!/usr/bin/env python3
"""Write a minimal x86-64 Windows console exe that prints a line and exits.

It imports only KERNEL32!GetStdHandle, WriteFile and ExitProcess, so it
exercises the loader's PE mapping, import thunks, guest->host calls and the
return to the host without needing a game or a Windows toolchain.

With --crash it writes to address 0 after printing instead of exiting, to
exercise the loader's fault handling (signal context fidelity under emulation).

With --veh the guest registers a vectored exception handler (written in guest
code), prints, faults on a null write, and the handler advances the context's
Rip past the faulting instruction and returns EXCEPTION_CONTINUE_EXECUTION. It
then prints "recovered" and exits. This exercises fault -> host -> guest handler
-> resume, which is where an embedded x86 emulator is most likely to differ.

With --thread the guest creates a second thread (CreateThread) that prints a line and returns 42, waits for it
(WaitForSingleObject) and exits with its exit code (GetExitCodeThread). This exercises thread start, which for an
emulated guest needs its own guest stack and a top-level host -> guest call.

With --com the guest imports D3D11CreateDevice, then calls AddRef and Release through the returned device's vtable
(function pointers built by the loader). Exit code = (AddRef result << 8) | Release result. This exercises a guest
calling host-implemented COM methods, which an emulated guest can only do through the loader's host-call mechanism.

usage: make_test_pe.py [--crash | --veh | --com | --thread] [output.exe]
"""
import struct
import sys

CRASH = "--crash" in sys.argv
VEH = "--veh" in sys.argv
COM = "--com" in sys.argv
THREAD = "--thread" in sys.argv
args = [a for a in sys.argv[1:] if a not in ("--crash", "--veh", "--com", "--thread")]
IMAGE_BASE = 0x140000000
TEXT_RVA, IDATA_RVA = 0x1000, 0x2000
FILE_ALIGN = 0x200
MESSAGE = b"hello from a guest exe\r\n"

# ---- .idata: import descriptors + per-DLL ILT/IAT + names
funcs = [b"GetStdHandle", b"WriteFile", b"ExitProcess"]
if VEH:
    funcs.append(b"AddVectoredExceptionHandler")
if THREAD:
    funcs += [b"CreateThread", b"WaitForSingleObject", b"GetExitCodeThread"]
imports = [(b"KERNEL32.dll", funcs)]
if COM:
    imports.append((b"d3d11.dll", [b"D3D11CreateDevice"]))

nd = len(imports)
cursor = 0x30 if nd == 1 else (20 * (nd + 1) + 15) & ~15
tables = []
for _dll, fl in imports:
    ilt = cursor; cursor += 8 * (len(fl) + 1)
    iat_ = cursor; cursor += 8 * (len(fl) + 1)
    tables.append((ilt, iat_))

idata = bytearray(0x200)
hn_off = cursor
iat = {}
for k, (dll, fl) in enumerate(imports):
    ilt_off, iat_off = tables[k]
    name_off = hn_off
    idata[name_off:name_off + len(dll) + 1] = dll + b"\0"
    hn_off = (name_off + len(dll) + 1 + 1) & ~1
    for i, name in enumerate(fl):
        rva = IDATA_RVA + hn_off
        entry = struct.pack("<H", 0) + name + b"\0"
        if len(entry) & 1:
            entry += b"\0"
        idata[hn_off:hn_off + len(entry)] = entry
        hn_off += len(entry)
        struct.pack_into("<Q", idata, ilt_off + 8 * i, rva)
        struct.pack_into("<Q", idata, iat_off + 8 * i, rva)
        iat[name] = IDATA_RVA + iat_off + 8 * i
    struct.pack_into("<IIIII", idata, 20 * k,
                     IDATA_RVA + ilt_off, 0, 0, IDATA_RVA + name_off, IDATA_RVA + iat_off)
assert hn_off <= 0x200, "import table does not fit in .idata"

# ---- .text
code = bytearray()


def emit(b):
    code.extend(b)


def rip_rel(opcode, target_rva):
    """Append `opcode` + rel32 to target_rva (rel32 is the last 4 bytes)."""
    emit(opcode)
    end_rva = TEXT_RVA + len(code) + 4
    emit(struct.pack("<i", target_rva - end_rva))


labels = {}
fixups = []

def rip_label(opcode, label):
    emit(opcode)
    fixups.append((len(code), label))
    emit(b"\0\0\0\0")

def print_msg(label, length):
    emit(b"\xb9\xf5\xff\xff\xff")                   # mov ecx, -11 (STD_OUTPUT_HANDLE)
    rip_rel(b"\xff\x15", iat[b"GetStdHandle"])      # call [GetStdHandle]
    emit(b"\x48\x89\xc1")                           # mov rcx, rax
    rip_label(b"\x48\x8d\x15", label)               # lea rdx, [rip+msg]
    emit(b"\x41\xb8" + struct.pack("<I", length))   # mov r8d, len
    emit(b"\x4c\x8d\x4c\x24\x30")                   # lea r9, [rsp+0x30]
    emit(b"\x48\xc7\x44\x24\x20\x00\x00\x00\x00")   # mov qword [rsp+0x20], 0
    rip_rel(b"\xff\x15", iat[b"WriteFile"])         # call [WriteFile]


if COM:
    emit(b"\x48\x83\xec\x68")                           # sub rsp, 0x68
    emit(b"\x31\xc9")                                    # xor ecx, ecx        pAdapter
    emit(b"\xba\x01\x00\x00\x00")                        # mov edx, 1          D3D_DRIVER_TYPE_HARDWARE
    emit(b"\x45\x31\xc0")                                # xor r8d, r8d        Software
    emit(b"\x45\x31\xc9")                                # xor r9d, r9d        Flags
    # D3D11CreateDevice has ten parameters; 5..10 are on the stack above the 0x20-byte shadow space.
    emit(b"\x48\xc7\x44\x24\x20\x00\x00\x00\x00")        # mov qword [rsp+0x20], 0   5  pFeatureLevels
    emit(b"\xc7\x44\x24\x28\x00\x00\x00\x00")            # mov dword [rsp+0x28], 0   6  FeatureLevels
    emit(b"\xc7\x44\x24\x30\x07\x00\x00\x00")            # mov dword [rsp+0x30], 7   7  SDKVersion
    emit(b"\x48\x8d\x44\x24\x60")                        # lea rax, [rsp+0x60]
    emit(b"\x48\x89\x44\x24\x38")                        # mov [rsp+0x38], rax       8  ppDevice
    emit(b"\x48\xc7\x44\x24\x40\x00\x00\x00\x00")        # mov qword [rsp+0x40], 0   9  pFeatureLevel
    emit(b"\x48\x8d\x44\x24\x58")                        # lea rax, [rsp+0x58]
    emit(b"\x48\x89\x44\x24\x48")                        # mov [rsp+0x48], rax       10 ppImmediateContext
    rip_rel(b"\xff\x15", iat[b"D3D11CreateDevice"])        # call [D3D11CreateDevice]
    emit(b"\x48\x8b\x4c\x24\x60")                        # mov rcx, [rsp+0x60]       device
    emit(b"\x48\x8b\x01")                                # mov rax, [rcx]            vtable
    emit(b"\xff\x50\x08")                                # call [rax+8]              AddRef
    emit(b"\x41\x89\xc4")                                # mov r12d, eax
    emit(b"\x48\x8b\x4c\x24\x60")                        # mov rcx, [rsp+0x60]
    emit(b"\x48\x8b\x01")                                # mov rax, [rcx]
    emit(b"\xff\x50\x10")                                # call [rax+0x10]           Release
    emit(b"\x41\x89\xc5")                                # mov r13d, eax
    emit(b"\x44\x89\xe1")                                # mov ecx, r12d
    emit(b"\xc1\xe1\x08")                                # shl ecx, 8
    emit(b"\x44\x01\xe9")                                # add ecx, r13d             exit code = (AddRef << 8) | Release
    rip_rel(b"\xff\x15", iat[b"ExitProcess"])             # call [ExitProcess]
    emit(b"\xcc")
    text = bytes(code).ljust(0x200, b"\0")
elif THREAD:
    emit(b"\x48\x83\xec\x58")                           # sub rsp, 0x58
    emit(b"\x31\xc9")                                    # xor ecx, ecx        lpThreadAttributes
    emit(b"\x31\xd2")                                    # xor edx, edx        dwStackSize
    rip_label(b"\x4c\x8d\x05", "threadfn")               # lea r8, [rip+threadfn]   lpStartAddress
    emit(b"\x45\x31\xc9")                                # xor r9d, r9d        lpParameter
    emit(b"\x48\xc7\x44\x24\x20\x00\x00\x00\x00")        # mov qword [rsp+0x20], 0  dwCreationFlags
    emit(b"\x48\xc7\x44\x24\x28\x00\x00\x00\x00")        # mov qword [rsp+0x28], 0  lpThreadId
    rip_rel(b"\xff\x15", iat[b"CreateThread"])            # call [CreateThread]
    emit(b"\x48\x89\xc3")                                # mov rbx, rax        thread handle
    emit(b"\x48\x89\xd9")                                # mov rcx, rbx
    emit(b"\xba\xff\xff\xff\xff")                        # mov edx, 0xFFFFFFFF (INFINITE)
    rip_rel(b"\xff\x15", iat[b"WaitForSingleObject"])     # call [WaitForSingleObject]
    emit(b"\x48\x89\xd9")                                # mov rcx, rbx
    emit(b"\x48\x8d\x54\x24\x30")                        # lea rdx, [rsp+0x30]
    rip_rel(b"\xff\x15", iat[b"GetExitCodeThread"])       # call [GetExitCodeThread]
    emit(b"\x8b\x4c\x24\x30")                            # mov ecx, [rsp+0x30]
    rip_rel(b"\xff\x15", iat[b"ExitProcess"])             # call [ExitProcess]
    emit(b"\xcc")
    labels["threadfn"] = len(code)                      # DWORD WINAPI threadfn(LPVOID)
    emit(b"\x48\x83\xec\x38")                           # sub rsp, 0x38
    print_msg("tmsg", len(b"hello from a guest thread\r\n"))
    emit(b"\xb8\x2a\x00\x00\x00")                        # mov eax, 42
    emit(b"\x48\x83\xc4\x38")                            # add rsp, 0x38
    emit(b"\xc3")                                         # ret
    labels["tmsg"] = len(code)
    emit(b"hello from a guest thread\r\n")
    for pos, label in fixups:
        struct.pack_into("<i", code, pos, (TEXT_RVA + labels[label]) - (TEXT_RVA + pos + 4))
    text = bytes(code).ljust(0x200, b"\0")
elif VEH:
    MSG1 = b"before fault\r\n"
    MSG2 = b"recovered after fault\r\n"
    emit(b"\x48\x83\xec\x38")                           # sub rsp, 0x38
    emit(b"\xb9\x01\x00\x00\x00")                       # mov ecx, 1 (call first)
    rip_label(b"\x48\x8d\x15", "handler")               # lea rdx, [rip+handler]
    rip_rel(b"\xff\x15", iat[b"AddVectoredExceptionHandler"])
    print_msg("msg1", len(MSG1))
    emit(b"\x31\xc0")                                   # xor eax, eax
    emit(b"\xc7\x00\x01\x00\x00\x00")                   # mov dword [rax], 1  -> access violation (6 bytes)
    print_msg("msg2", len(MSG2))
    emit(b"\x31\xc9")                                   # xor ecx, ecx
    rip_rel(b"\xff\x15", iat[b"ExitProcess"])           # call [ExitProcess]
    emit(b"\xcc")                                       # int3 (not reached)
    labels["handler"] = len(code)                       # LONG handler(EXCEPTION_POINTERS *rcx)
    emit(b"\x48\x8b\x41\x08")                           # mov rax, [rcx+8]       ; ContextRecord
    emit(b"\x48\x83\x80\xf8\x00\x00\x00\x06")           # add qword [rax+0xF8], 6 ; Rip += sizeof(mov [rax],1)
    emit(b"\xb8\xff\xff\xff\xff")                       # mov eax, -1 (EXCEPTION_CONTINUE_EXECUTION)
    emit(b"\xc3")                                       # ret
    labels["msg1"] = len(code)
    emit(MSG1)
    labels["msg2"] = len(code)
    emit(MSG2)
    for pos, label in fixups:
        struct.pack_into("<i", code, pos, (TEXT_RVA + labels[label]) - (TEXT_RVA + pos + 4))
else:
    emit(b"\x48\x83\xec\x38")                       # sub rsp, 0x38
    emit(b"\xb9\xf5\xff\xff\xff")                   # mov ecx, -11 (STD_OUTPUT_HANDLE)
    rip_rel(b"\xff\x15", iat[b"GetStdHandle"])      # call [GetStdHandle]
    emit(b"\x48\x89\xc1")                           # mov rcx, rax
    msg_fixup = len(code) + 3
    rip_rel(b"\x48\x8d\x15", 0)                     # lea rdx, [rip+msg] (patched below)
    emit(b"\x41\xb8" + struct.pack("<I", len(MESSAGE)))  # mov r8d, len
    emit(b"\x4c\x8d\x4c\x24\x30")                   # lea r9, [rsp+0x30]
    emit(b"\x48\xc7\x44\x24\x20\x00\x00\x00\x00")   # mov qword [rsp+0x20], 0
    rip_rel(b"\xff\x15", iat[b"WriteFile"])         # call [WriteFile]
    if CRASH:
        emit(b"\x31\xc0")                           # xor eax, eax
        emit(b"\xc7\x00\x01\x00\x00\x00")           # mov dword [rax], 1  -> access violation
    else:
        emit(b"\x31\xc9")                           # xor ecx, ecx
        rip_rel(b"\xff\x15", iat[b"ExitProcess"])   # call [ExitProcess]
    emit(b"\xcc")                                   # int3 (not reached)
    msg_rva = TEXT_RVA + len(code)
    emit(MESSAGE)
    struct.pack_into("<i", code, msg_fixup, msg_rva - (TEXT_RVA + msg_fixup + 4))
text = bytes(code).ljust(0x200, b"\0")

# ---- headers
size_of_image = 0x3000
dos = bytearray(0x40)
dos[0:2] = b"MZ"
struct.pack_into("<I", dos, 0x3c, 0x40)

coff = struct.pack("<HHIIIHH", 0x8664, 2, 0, 0, 0, 0xF0, 0x0022)
opt = struct.pack(
    "<HBBIIIIIQIIHHHHHHIIIIHHQQQQII",
    0x20b, 14, 0,
    0x200, 0x200, 0,                 # SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData
    TEXT_RVA, TEXT_RVA,              # AddressOfEntryPoint, BaseOfCode
    IMAGE_BASE,
    0x1000, FILE_ALIGN,
    6, 0, 0, 0, 6, 0,                # OS, image, subsystem versions
    0, size_of_image, 0x200, 0,
    3, 0x8160,                       # Subsystem = console, DllCharacteristics
    0x100000, 0x1000, 0x100000, 0x1000,
    0, 16)
dirs = bytearray(16 * 8)
struct.pack_into("<II", dirs, 8, IDATA_RVA, 0x80 if nd == 1 else 20 * (nd + 1))   # import directory

def section(name, vsize, rva, rawsize, rawptr, flags):
    return struct.pack("<8sIIIIIIHHI", name, vsize, rva, rawsize, rawptr, 0, 0, 0, 0, flags)

sections = (section(b".text", len(code), TEXT_RVA, 0x200, 0x200, 0x60000020)
            + section(b".idata", 0x200, IDATA_RVA, 0x200, 0x400, 0xC0000040))

headers = bytes(dos) + b"PE\0\0" + coff + opt + bytes(dirs) + sections
assert len(headers) <= 0x200
image = headers.ljust(0x200, b"\0") + text + bytes(idata)

out = args[0] if args else "hello.exe"
with open(out, "wb") as f:
    f.write(image)
print(f"wrote {out} ({len(image)} bytes)")
