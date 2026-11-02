#define _GNU_SOURCE
#include "win32_syscall.h"

#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef PR_SET_SYSCALL_USER_DISPATCH
#define PR_SET_SYSCALL_USER_DISPATCH 59
#endif
#ifndef PR_SYS_DISPATCH_ON
#define PR_SYS_DISPATCH_ON 1
#endif
#ifndef SYS_USER_DISPATCH
#define SYS_USER_DISPATCH 2
#endif

__thread uint8_t beer_win32_syscall_selector;
__thread uint64_t beer_win32_syscall_resume_rip;

/* Re-enter guest code after a serviced call: block guest system calls again
 * and continue at the saved address. No register or flag is changed. */
extern void beer_win32_syscall_resume(void);
__asm__(
    ".text\n"
    ".global beer_win32_syscall_resume\n"
    "beer_win32_syscall_resume:\n"
    "    movb $1, %fs:beer_win32_syscall_selector@tpoff\n"
    "    jmp *%fs:beer_win32_syscall_resume_rip@tpoff\n"
);

static BeerWin32ContinueFn g_continue;

void beer_win32_syscall_set_continue(BeerWin32ContinueFn fn) { g_continue = fn; }

typedef struct {
    char name[64];              /* canonical Nt<Name>; empty when unused */
    BeerWin32SyscallFn fn;
    int reported;
} BeerWin32Service;

static BeerWin32Service g_services[BEER_WIN32_SYSCALL_CAPACITY];
static unsigned g_next_extra = BEER_WIN32_SYSCALL_EXTRA_BASE;
static int g_installed;
static int g_trace = -1;

/* System-service numbers of Windows 10 build 19045 (22H2, x64), complete: every Nt/Zw stub Beer exports
 * carries the number real ntdll has. Protection code reads the numbers from ntdll (or from a mapping of
 * ntdll.dll) and can check them against the build it reports. Builds 19041 through 19045 share this table.
 * Source: j00ru/windows-syscalls, x64 nt.csv, "Windows 10 (22H2)". */
static const struct { const char *name; uint16_t number; } g_windows_19045[] = {
    {"NtAccessCheck", 0x000}, {"NtWorkerFactoryWorkerReady", 0x001}, {"NtAcceptConnectPort", 0x002},
    {"NtMapUserPhysicalPagesScatter", 0x003}, {"NtWaitForSingleObject", 0x004}, {"NtCallbackReturn", 0x005},
    {"NtReadFile", 0x006}, {"NtDeviceIoControlFile", 0x007}, {"NtWriteFile", 0x008},
    {"NtRemoveIoCompletion", 0x009}, {"NtReleaseSemaphore", 0x00A}, {"NtReplyWaitReceivePort", 0x00B},
    {"NtReplyPort", 0x00C}, {"NtSetInformationThread", 0x00D}, {"NtSetEvent", 0x00E}, {"NtClose", 0x00F},
    {"NtQueryObject", 0x010}, {"NtQueryInformationFile", 0x011}, {"NtOpenKey", 0x012},
    {"NtEnumerateValueKey", 0x013}, {"NtFindAtom", 0x014}, {"NtQueryDefaultLocale", 0x015}, {"NtQueryKey", 0x016},
    {"NtQueryValueKey", 0x017}, {"NtAllocateVirtualMemory", 0x018}, {"NtQueryInformationProcess", 0x019},
    {"NtWaitForMultipleObjects32", 0x01A}, {"NtWriteFileGather", 0x01B}, {"NtSetInformationProcess", 0x01C},
    {"NtCreateKey", 0x01D}, {"NtFreeVirtualMemory", 0x01E}, {"NtImpersonateClientOfPort", 0x01F},
    {"NtReleaseMutant", 0x020}, {"NtQueryInformationToken", 0x021}, {"NtRequestWaitReplyPort", 0x022},
    {"NtQueryVirtualMemory", 0x023}, {"NtOpenThreadToken", 0x024}, {"NtQueryInformationThread", 0x025},
    {"NtOpenProcess", 0x026}, {"NtSetInformationFile", 0x027}, {"NtMapViewOfSection", 0x028},
    {"NtAccessCheckAndAuditAlarm", 0x029}, {"NtUnmapViewOfSection", 0x02A}, {"NtReplyWaitReceivePortEx", 0x02B},
    {"NtTerminateProcess", 0x02C}, {"NtSetEventBoostPriority", 0x02D}, {"NtReadFileScatter", 0x02E},
    {"NtOpenThreadTokenEx", 0x02F}, {"NtOpenProcessTokenEx", 0x030}, {"NtQueryPerformanceCounter", 0x031},
    {"NtEnumerateKey", 0x032}, {"NtOpenFile", 0x033}, {"NtDelayExecution", 0x034}, {"NtQueryDirectoryFile", 0x035},
    {"NtQuerySystemInformation", 0x036}, {"NtOpenSection", 0x037}, {"NtQueryTimer", 0x038},
    {"NtFsControlFile", 0x039}, {"NtWriteVirtualMemory", 0x03A}, {"NtCloseObjectAuditAlarm", 0x03B},
    {"NtDuplicateObject", 0x03C}, {"NtQueryAttributesFile", 0x03D}, {"NtClearEvent", 0x03E},
    {"NtReadVirtualMemory", 0x03F}, {"NtOpenEvent", 0x040}, {"NtAdjustPrivilegesToken", 0x041},
    {"NtDuplicateToken", 0x042}, {"NtContinue", 0x043}, {"NtQueryDefaultUILanguage", 0x044},
    {"NtQueueApcThread", 0x045}, {"NtYieldExecution", 0x046}, {"NtAddAtom", 0x047}, {"NtCreateEvent", 0x048},
    {"NtQueryVolumeInformationFile", 0x049}, {"NtCreateSection", 0x04A}, {"NtFlushBuffersFile", 0x04B},
    {"NtApphelpCacheControl", 0x04C}, {"NtCreateProcessEx", 0x04D}, {"NtCreateThread", 0x04E},
    {"NtIsProcessInJob", 0x04F}, {"NtProtectVirtualMemory", 0x050}, {"NtQuerySection", 0x051},
    {"NtResumeThread", 0x052}, {"NtTerminateThread", 0x053}, {"NtReadRequestData", 0x054}, {"NtCreateFile", 0x055},
    {"NtQueryEvent", 0x056}, {"NtWriteRequestData", 0x057}, {"NtOpenDirectoryObject", 0x058},
    {"NtAccessCheckByTypeAndAuditAlarm", 0x059}, {"NtQuerySystemTime", 0x05A}, {"NtWaitForMultipleObjects", 0x05B},
    {"NtSetInformationObject", 0x05C}, {"NtCancelIoFile", 0x05D}, {"NtTraceEvent", 0x05E},
    {"NtPowerInformation", 0x05F}, {"NtSetValueKey", 0x060}, {"NtCancelTimer", 0x061}, {"NtSetTimer", 0x062},
    {"NtAccessCheckByType", 0x063}, {"NtAccessCheckByTypeResultList", 0x064},
    {"NtAccessCheckByTypeResultListAndAuditAlarm", 0x065},
    {"NtAccessCheckByTypeResultListAndAuditAlarmByHandle", 0x066}, {"NtAcquireCrossVmMutant", 0x067},
    {"NtAcquireProcessActivityReference", 0x068}, {"NtAddAtomEx", 0x069}, {"NtAddBootEntry", 0x06A},
    {"NtAddDriverEntry", 0x06B}, {"NtAdjustGroupsToken", 0x06C}, {"NtAdjustTokenClaimsAndDeviceGroups", 0x06D},
    {"NtAlertResumeThread", 0x06E}, {"NtAlertThread", 0x06F}, {"NtAlertThreadByThreadId", 0x070},
    {"NtAllocateLocallyUniqueId", 0x071}, {"NtAllocateReserveObject", 0x072},
    {"NtAllocateUserPhysicalPages", 0x073}, {"NtAllocateUserPhysicalPagesEx", 0x074}, {"NtAllocateUuids", 0x075},
    {"NtAllocateVirtualMemoryEx", 0x076}, {"NtAlpcAcceptConnectPort", 0x077}, {"NtAlpcCancelMessage", 0x078},
    {"NtAlpcConnectPort", 0x079}, {"NtAlpcConnectPortEx", 0x07A}, {"NtAlpcCreatePort", 0x07B},
    {"NtAlpcCreatePortSection", 0x07C}, {"NtAlpcCreateResourceReserve", 0x07D}, {"NtAlpcCreateSectionView", 0x07E},
    {"NtAlpcCreateSecurityContext", 0x07F}, {"NtAlpcDeletePortSection", 0x080},
    {"NtAlpcDeleteResourceReserve", 0x081}, {"NtAlpcDeleteSectionView", 0x082},
    {"NtAlpcDeleteSecurityContext", 0x083}, {"NtAlpcDisconnectPort", 0x084},
    {"NtAlpcImpersonateClientContainerOfPort", 0x085}, {"NtAlpcImpersonateClientOfPort", 0x086},
    {"NtAlpcOpenSenderProcess", 0x087}, {"NtAlpcOpenSenderThread", 0x088}, {"NtAlpcQueryInformation", 0x089},
    {"NtAlpcQueryInformationMessage", 0x08A}, {"NtAlpcRevokeSecurityContext", 0x08B},
    {"NtAlpcSendWaitReceivePort", 0x08C}, {"NtAlpcSetInformation", 0x08D}, {"NtAreMappedFilesTheSame", 0x08E},
    {"NtAssignProcessToJobObject", 0x08F}, {"NtAssociateWaitCompletionPacket", 0x090}, {"NtCallEnclave", 0x091},
    {"NtCancelIoFileEx", 0x092}, {"NtCancelSynchronousIoFile", 0x093}, {"NtCancelTimer2", 0x094},
    {"NtCancelWaitCompletionPacket", 0x095}, {"NtCommitComplete", 0x096}, {"NtCommitEnlistment", 0x097},
    {"NtCommitRegistryTransaction", 0x098}, {"NtCommitTransaction", 0x099}, {"NtCompactKeys", 0x09A},
    {"NtCompareObjects", 0x09B}, {"NtCompareSigningLevels", 0x09C}, {"NtCompareTokens", 0x09D},
    {"NtCompleteConnectPort", 0x09E}, {"NtCompressKey", 0x09F}, {"NtConnectPort", 0x0A0}, {"NtContinueEx", 0x0A1},
    {"NtConvertBetweenAuxiliaryCounterAndPerformanceCounter", 0x0A2}, {"NtCopyFileChunk", 0x0A3},
    {"NtCreateCrossVmEvent", 0x0A4}, {"NtCreateCrossVmMutant", 0x0A5}, {"NtCreateDebugObject", 0x0A6},
    {"NtCreateDirectoryObject", 0x0A7}, {"NtCreateDirectoryObjectEx", 0x0A8}, {"NtCreateEnclave", 0x0A9},
    {"NtCreateEnlistment", 0x0AA}, {"NtCreateEventPair", 0x0AB}, {"NtCreateIRTimer", 0x0AC},
    {"NtCreateIoCompletion", 0x0AD}, {"NtCreateJobObject", 0x0AE}, {"NtCreateJobSet", 0x0AF},
    {"NtCreateKeyTransacted", 0x0B0}, {"NtCreateKeyedEvent", 0x0B1}, {"NtCreateLowBoxToken", 0x0B2},
    {"NtCreateMailslotFile", 0x0B3}, {"NtCreateMutant", 0x0B4}, {"NtCreateNamedPipeFile", 0x0B5},
    {"NtCreatePagingFile", 0x0B6}, {"NtCreatePartition", 0x0B7}, {"NtCreatePort", 0x0B8},
    {"NtCreatePrivateNamespace", 0x0B9}, {"NtCreateProcess", 0x0BA}, {"NtCreateProfile", 0x0BB},
    {"NtCreateProfileEx", 0x0BC}, {"NtCreateRegistryTransaction", 0x0BD}, {"NtCreateResourceManager", 0x0BE},
    {"NtCreateSectionEx", 0x0BF}, {"NtCreateSemaphore", 0x0C0}, {"NtCreateSymbolicLinkObject", 0x0C1},
    {"NtCreateThreadEx", 0x0C2}, {"NtCreateTimer", 0x0C3}, {"NtCreateTimer2", 0x0C4}, {"NtCreateToken", 0x0C5},
    {"NtCreateTokenEx", 0x0C6}, {"NtCreateTransaction", 0x0C7}, {"NtCreateTransactionManager", 0x0C8},
    {"NtCreateUserProcess", 0x0C9}, {"NtCreateWaitCompletionPacket", 0x0CA}, {"NtCreateWaitablePort", 0x0CB},
    {"NtCreateWnfStateName", 0x0CC}, {"NtCreateWorkerFactory", 0x0CD}, {"NtDebugActiveProcess", 0x0CE},
    {"NtDebugContinue", 0x0CF}, {"NtDeleteAtom", 0x0D0}, {"NtDeleteBootEntry", 0x0D1},
    {"NtDeleteDriverEntry", 0x0D2}, {"NtDeleteFile", 0x0D3}, {"NtDeleteKey", 0x0D4},
    {"NtDeleteObjectAuditAlarm", 0x0D5}, {"NtDeletePrivateNamespace", 0x0D6}, {"NtDeleteValueKey", 0x0D7},
    {"NtDeleteWnfStateData", 0x0D8}, {"NtDeleteWnfStateName", 0x0D9}, {"NtDirectGraphicsCall", 0x0DA},
    {"NtDisableLastKnownGood", 0x0DB}, {"NtDisplayString", 0x0DC}, {"NtDrawText", 0x0DD},
    {"NtEnableLastKnownGood", 0x0DE}, {"NtEnumerateBootEntries", 0x0DF}, {"NtEnumerateDriverEntries", 0x0E0},
    {"NtEnumerateSystemEnvironmentValuesEx", 0x0E1}, {"NtEnumerateTransactionObject", 0x0E2},
    {"NtExtendSection", 0x0E3}, {"NtFilterBootOption", 0x0E4}, {"NtFilterToken", 0x0E5}, {"NtFilterTokenEx", 0x0E6},
    {"NtFlushBuffersFileEx", 0x0E7}, {"NtFlushInstallUILanguage", 0x0E8}, {"NtFlushInstructionCache", 0x0E9},
    {"NtFlushKey", 0x0EA}, {"NtFlushProcessWriteBuffers", 0x0EB}, {"NtFlushVirtualMemory", 0x0EC},
    {"NtFlushWriteBuffer", 0x0ED}, {"NtFreeUserPhysicalPages", 0x0EE}, {"NtFreezeRegistry", 0x0EF},
    {"NtFreezeTransactions", 0x0F0}, {"NtGetCachedSigningLevel", 0x0F1},
    {"NtGetCompleteWnfStateSubscription", 0x0F2}, {"NtGetContextThread", 0x0F3},
    {"NtGetCurrentProcessorNumber", 0x0F4}, {"NtGetCurrentProcessorNumberEx", 0x0F5},
    {"NtGetDevicePowerState", 0x0F6}, {"NtGetMUIRegistryInfo", 0x0F7}, {"NtGetNextProcess", 0x0F8},
    {"NtGetNextThread", 0x0F9}, {"NtGetNlsSectionPtr", 0x0FA}, {"NtGetNotificationResourceManager", 0x0FB},
    {"NtGetWriteWatch", 0x0FC}, {"NtImpersonateAnonymousToken", 0x0FD}, {"NtImpersonateThread", 0x0FE},
    {"NtInitializeEnclave", 0x0FF}, {"NtInitializeNlsFiles", 0x100}, {"NtInitializeRegistry", 0x101},
    {"NtInitiatePowerAction", 0x102}, {"NtIsSystemResumeAutomatic", 0x103}, {"NtIsUILanguageComitted", 0x104},
    {"NtListenPort", 0x105}, {"NtLoadDriver", 0x106}, {"NtLoadEnclaveData", 0x107}, {"NtLoadKey", 0x108},
    {"NtLoadKey2", 0x109}, {"NtLoadKeyEx", 0x10A}, {"NtLockFile", 0x10B}, {"NtLockProductActivationKeys", 0x10C},
    {"NtLockRegistryKey", 0x10D}, {"NtLockVirtualMemory", 0x10E}, {"NtMakePermanentObject", 0x10F},
    {"NtMakeTemporaryObject", 0x110}, {"NtManageHotPatch", 0x111}, {"NtManagePartition", 0x112},
    {"NtMapCMFModule", 0x113}, {"NtMapUserPhysicalPages", 0x114}, {"NtMapViewOfSectionEx", 0x115},
    {"NtModifyBootEntry", 0x116}, {"NtModifyDriverEntry", 0x117}, {"NtNotifyChangeDirectoryFile", 0x118},
    {"NtNotifyChangeDirectoryFileEx", 0x119}, {"NtNotifyChangeKey", 0x11A}, {"NtNotifyChangeMultipleKeys", 0x11B},
    {"NtNotifyChangeSession", 0x11C}, {"NtOpenEnlistment", 0x11D}, {"NtOpenEventPair", 0x11E},
    {"NtOpenIoCompletion", 0x11F}, {"NtOpenJobObject", 0x120}, {"NtOpenKeyEx", 0x121},
    {"NtOpenKeyTransacted", 0x122}, {"NtOpenKeyTransactedEx", 0x123}, {"NtOpenKeyedEvent", 0x124},
    {"NtOpenMutant", 0x125}, {"NtOpenObjectAuditAlarm", 0x126}, {"NtOpenPartition", 0x127},
    {"NtOpenPrivateNamespace", 0x128}, {"NtOpenProcessToken", 0x129}, {"NtOpenRegistryTransaction", 0x12A},
    {"NtOpenResourceManager", 0x12B}, {"NtOpenSemaphore", 0x12C}, {"NtOpenSession", 0x12D},
    {"NtOpenSymbolicLinkObject", 0x12E}, {"NtOpenThread", 0x12F}, {"NtOpenTimer", 0x130},
    {"NtOpenTransaction", 0x131}, {"NtOpenTransactionManager", 0x132}, {"NtPlugPlayControl", 0x133},
    {"NtPrePrepareComplete", 0x134}, {"NtPrePrepareEnlistment", 0x135}, {"NtPrepareComplete", 0x136},
    {"NtPrepareEnlistment", 0x137}, {"NtPrivilegeCheck", 0x138}, {"NtPrivilegeObjectAuditAlarm", 0x139},
    {"NtPrivilegedServiceAuditAlarm", 0x13A}, {"NtPropagationComplete", 0x13B}, {"NtPropagationFailed", 0x13C},
    {"NtPssCaptureVaSpaceBulk", 0x13D}, {"NtPulseEvent", 0x13E}, {"NtQueryAuxiliaryCounterFrequency", 0x13F},
    {"NtQueryBootEntryOrder", 0x140}, {"NtQueryBootOptions", 0x141}, {"NtQueryDebugFilterState", 0x142},
    {"NtQueryDirectoryFileEx", 0x143}, {"NtQueryDirectoryObject", 0x144}, {"NtQueryDriverEntryOrder", 0x145},
    {"NtQueryEaFile", 0x146}, {"NtQueryFullAttributesFile", 0x147}, {"NtQueryInformationAtom", 0x148},
    {"NtQueryInformationByName", 0x149}, {"NtQueryInformationEnlistment", 0x14A},
    {"NtQueryInformationJobObject", 0x14B}, {"NtQueryInformationPort", 0x14C},
    {"NtQueryInformationResourceManager", 0x14D}, {"NtQueryInformationTransaction", 0x14E},
    {"NtQueryInformationTransactionManager", 0x14F}, {"NtQueryInformationWorkerFactory", 0x150},
    {"NtQueryInstallUILanguage", 0x151}, {"NtQueryIntervalProfile", 0x152}, {"NtQueryIoCompletion", 0x153},
    {"NtQueryLicenseValue", 0x154}, {"NtQueryMultipleValueKey", 0x155}, {"NtQueryMutant", 0x156},
    {"NtQueryOpenSubKeys", 0x157}, {"NtQueryOpenSubKeysEx", 0x158}, {"NtQueryPortInformationProcess", 0x159},
    {"NtQueryQuotaInformationFile", 0x15A}, {"NtQuerySecurityAttributesToken", 0x15B},
    {"NtQuerySecurityObject", 0x15C}, {"NtQuerySecurityPolicy", 0x15D}, {"NtQuerySemaphore", 0x15E},
    {"NtQuerySymbolicLinkObject", 0x15F}, {"NtQuerySystemEnvironmentValue", 0x160},
    {"NtQuerySystemEnvironmentValueEx", 0x161}, {"NtQuerySystemInformationEx", 0x162},
    {"NtQueryTimerResolution", 0x163}, {"NtQueryWnfStateData", 0x164}, {"NtQueryWnfStateNameInformation", 0x165},
    {"NtQueueApcThreadEx", 0x166}, {"NtQueueApcThreadEx2", 0x167}, {"NtRaiseException", 0x168},
    {"NtRaiseHardError", 0x169}, {"NtReadOnlyEnlistment", 0x16A}, {"NtRecoverEnlistment", 0x16B},
    {"NtRecoverResourceManager", 0x16C}, {"NtRecoverTransactionManager", 0x16D},
    {"NtRegisterProtocolAddressInformation", 0x16E}, {"NtRegisterThreadTerminatePort", 0x16F},
    {"NtReleaseKeyedEvent", 0x170}, {"NtReleaseWorkerFactoryWorker", 0x171}, {"NtRemoveIoCompletionEx", 0x172},
    {"NtRemoveProcessDebug", 0x173}, {"NtRenameKey", 0x174}, {"NtRenameTransactionManager", 0x175},
    {"NtReplaceKey", 0x176}, {"NtReplacePartitionUnit", 0x177}, {"NtReplyWaitReplyPort", 0x178},
    {"NtRequestPort", 0x179}, {"NtResetEvent", 0x17A}, {"NtResetWriteWatch", 0x17B}, {"NtRestoreKey", 0x17C},
    {"NtResumeProcess", 0x17D}, {"NtRevertContainerImpersonation", 0x17E}, {"NtRollbackComplete", 0x17F},
    {"NtRollbackEnlistment", 0x180}, {"NtRollbackRegistryTransaction", 0x181}, {"NtRollbackTransaction", 0x182},
    {"NtRollforwardTransactionManager", 0x183}, {"NtSaveKey", 0x184}, {"NtSaveKeyEx", 0x185},
    {"NtSaveMergedKeys", 0x186}, {"NtSecureConnectPort", 0x187}, {"NtSerializeBoot", 0x188},
    {"NtSetBootEntryOrder", 0x189}, {"NtSetBootOptions", 0x18A}, {"NtSetCachedSigningLevel", 0x18B},
    {"NtSetCachedSigningLevel2", 0x18C}, {"NtSetContextThread", 0x18D}, {"NtSetDebugFilterState", 0x18E},
    {"NtSetDefaultHardErrorPort", 0x18F}, {"NtSetDefaultLocale", 0x190}, {"NtSetDefaultUILanguage", 0x191},
    {"NtSetDriverEntryOrder", 0x192}, {"NtSetEaFile", 0x193}, {"NtSetHighEventPair", 0x194},
    {"NtSetHighWaitLowEventPair", 0x195}, {"NtSetIRTimer", 0x196}, {"NtSetInformationDebugObject", 0x197},
    {"NtSetInformationEnlistment", 0x198}, {"NtSetInformationJobObject", 0x199}, {"NtSetInformationKey", 0x19A},
    {"NtSetInformationResourceManager", 0x19B}, {"NtSetInformationSymbolicLink", 0x19C},
    {"NtSetInformationToken", 0x19D}, {"NtSetInformationTransaction", 0x19E},
    {"NtSetInformationTransactionManager", 0x19F}, {"NtSetInformationVirtualMemory", 0x1A0},
    {"NtSetInformationWorkerFactory", 0x1A1}, {"NtSetIntervalProfile", 0x1A2}, {"NtSetIoCompletion", 0x1A3},
    {"NtSetIoCompletionEx", 0x1A4}, {"NtSetLdtEntries", 0x1A5}, {"NtSetLowEventPair", 0x1A6},
    {"NtSetLowWaitHighEventPair", 0x1A7}, {"NtSetQuotaInformationFile", 0x1A8}, {"NtSetSecurityObject", 0x1A9},
    {"NtSetSystemEnvironmentValue", 0x1AA}, {"NtSetSystemEnvironmentValueEx", 0x1AB},
    {"NtSetSystemInformation", 0x1AC}, {"NtSetSystemPowerState", 0x1AD}, {"NtSetSystemTime", 0x1AE},
    {"NtSetThreadExecutionState", 0x1AF}, {"NtSetTimer2", 0x1B0}, {"NtSetTimerEx", 0x1B1},
    {"NtSetTimerResolution", 0x1B2}, {"NtSetUuidSeed", 0x1B3}, {"NtSetVolumeInformationFile", 0x1B4},
    {"NtSetWnfProcessNotificationEvent", 0x1B5}, {"NtShutdownSystem", 0x1B6}, {"NtShutdownWorkerFactory", 0x1B7},
    {"NtSignalAndWaitForSingleObject", 0x1B8}, {"NtSinglePhaseReject", 0x1B9}, {"NtStartProfile", 0x1BA},
    {"NtStopProfile", 0x1BB}, {"NtSubscribeWnfStateChange", 0x1BC}, {"NtSuspendProcess", 0x1BD},
    {"NtSuspendThread", 0x1BE}, {"NtSystemDebugControl", 0x1BF}, {"NtTerminateEnclave", 0x1C0},
    {"NtTerminateJobObject", 0x1C1}, {"NtTestAlert", 0x1C2}, {"NtThawRegistry", 0x1C3},
    {"NtThawTransactions", 0x1C4}, {"NtTraceControl", 0x1C5}, {"NtTranslateFilePath", 0x1C6},
    {"NtUmsThreadYield", 0x1C7}, {"NtUnloadDriver", 0x1C8}, {"NtUnloadKey", 0x1C9}, {"NtUnloadKey2", 0x1CA},
    {"NtUnloadKeyEx", 0x1CB}, {"NtUnlockFile", 0x1CC}, {"NtUnlockVirtualMemory", 0x1CD},
    {"NtUnmapViewOfSectionEx", 0x1CE}, {"NtUnsubscribeWnfStateChange", 0x1CF}, {"NtUpdateWnfStateData", 0x1D0},
    {"NtVdmControl", 0x1D1}, {"NtWaitForAlertByThreadId", 0x1D2}, {"NtWaitForDebugEvent", 0x1D3},
    {"NtWaitForKeyedEvent", 0x1D4}, {"NtWaitForWorkViaWorkerFactory", 0x1D5}, {"NtWaitHighEventPair", 0x1D6},
    {"NtLoadKey3", 0x1D7}, {"NtWaitLowEventPair", 0x1D8},
};

int beer_win32_syscall_is_service_name(const char *name)
{
    if (!name) return 0;
    if (strncmp(name, "Nt", 2) && strncmp(name, "Zw", 2)) return 0;
    if (!strncmp(name, "Ntdll", 5)) return 0;               /* NtdllDefWindowProc_A and similar */
    if (!strcmp(name + 2, "GetTickCount") || !strcmp(name + 2, "CurrentTeb")) return 0;
    return name[2] >= 'A' && name[2] <= 'Z';
}

static char g_service_aliases[BEER_WIN32_SYSCALL_CAPACITY][64];
static int g_catalog_initialized;

static void service_catalog_init(void)
{
    if (g_catalog_initialized) return;
    _Static_assert(sizeof(g_windows_19045) / sizeof(*g_windows_19045) ==
                   BEER_WIN32_SYSCALL_EXTRA_BASE, "service prefix must be contiguous");
    for (size_t i = 0; i < sizeof(g_windows_19045) / sizeof(*g_windows_19045); ++i) {
        unsigned number = g_windows_19045[i].number;
        snprintf(g_services[number].name, sizeof(g_services[number].name), "%s", g_windows_19045[i].name);
        snprintf(g_service_aliases[number], sizeof(g_service_aliases[number]), "Zw%s", g_windows_19045[i].name + 2);
    }
    g_catalog_initialized = 1;
}

size_t beer_win32_syscall_count(void)
{
    service_catalog_init();
    return g_next_extra;
}

const char *beer_win32_syscall_name(uint32_t number, int zw_alias)
{
    service_catalog_init();
    if (number >= g_next_extra || !g_services[number].name[0]) return NULL;
    return zw_alias ? g_service_aliases[number] : g_services[number].name;
}

int beer_win32_syscall_supported(uint32_t number)
{
    service_catalog_init();
    return number < g_next_extra && g_services[number].fn != NULL;
}

int beer_win32_syscall_register(const char *name, BeerWin32SyscallFn fn)
{
    if (!beer_win32_syscall_is_service_name(name) || strlen(name) >= sizeof(g_services[0].name))
        return -1;
    char canonical[64];
    snprintf(canonical, sizeof(canonical), "Nt%s", name + 2);
    service_catalog_init();
    int number = -1;
    for (size_t i = 0; i < sizeof(g_windows_19045) / sizeof(*g_windows_19045); ++i)
        if (!strcmp(g_windows_19045[i].name, canonical)) number = g_windows_19045[i].number;
    if (number < 0) {
        for (unsigned i = BEER_WIN32_SYSCALL_EXTRA_BASE; i < g_next_extra; ++i)
            if (!strcmp(g_services[i].name, canonical)) number = (int)i;
    }
    if (number < 0) {
        if (g_next_extra >= BEER_WIN32_SYSCALL_CAPACITY) return -1;
        number = (int)g_next_extra++;
    }
    BeerWin32Service *service = &g_services[number];
    if (!service->name[0]) {
        snprintf(service->name, sizeof(service->name), "%s", canonical);
        snprintf(g_service_aliases[number], sizeof(g_service_aliases[number]), "Zw%s", canonical + 2);
    }
    if (!service->fn) service->fn = fn;
    return number;
}

size_t beer_win32_syscall_write_stub(uint8_t *out, size_t capacity, uint32_t number)
{
    static const uint8_t stub[BEER_WIN32_SYSCALL_STUB_SIZE] = {
        0x4C, 0x8B, 0xD1,                               /* mov r10, rcx */
        0xB8, 0, 0, 0, 0,                               /* mov eax, number */
        0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01, /* test byte ptr [7FFE0308h], 1 */
        0x75, 0x03,                                     /* jne int2e */
        0x0F, 0x05,                                     /* syscall */
        0xC3,                                           /* ret */
        0xCD, 0x2E,                                     /* int 2Eh */
        0xC3,                                           /* ret */
    };
    if (!out || capacity < sizeof(stub)) return 0;
    memcpy(out, stub, sizeof(stub));
    memcpy(out + 4, &number, sizeof(number));
    return sizeof(stub);
}

/* ---- Host or guest caller ----
 * Host code is the loader binary, shared objects and the vDSO. Everything else
 * that executes (PE images, guest allocations) is guest code. Ranges are cached
 * from /proc/self/maps, which is reread when an address is in no known range. */
typedef struct { uintptr_t start, end; uint8_t host; } BeerMapRange;
static BeerMapRange g_ranges[16384];
static _Atomic int g_range_count;
static atomic_flag g_ranges_lock = ATOMIC_FLAG_INIT;
static char g_exe_path[512];

static int mapping_is_host(const char *path)
{
    if (!path[0]) return 0;
    if (!strcmp(path, "[vdso]") || !strcmp(path, "[vsyscall]")) return 1;
    if (path[0] != '/') return 0;
    if (g_exe_path[0] && !strncmp(path, g_exe_path, strlen(g_exe_path))) return 1;
    for (const char *so = strstr(path, ".so"); so; so = strstr(so + 3, ".so"))
        if (so[3] == 0 || so[3] == '.' || so[3] == ' ') return 1;
    return 0;
}

static void ranges_refresh(void)
{
    while (atomic_flag_test_and_set_explicit(&g_ranges_lock, memory_order_acquire)) {}
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    int count = 0;
    static char buffer[1 << 16];
    static char line[1024];
    size_t line_length = 0;
    ssize_t got;
    while (fd >= 0 && (got = read(fd, buffer, sizeof(buffer))) > 0) {
        for (ssize_t i = 0; i < got; ++i) {
            char ch = buffer[i];
            if (ch != '\n') {
                if (line_length + 1 < sizeof(line)) line[line_length++] = ch;
                continue;
            }
            line[line_length] = 0;
            line_length = 0;
            unsigned long long start, end;
            char perms[8];
            int path_offset = 0;
            if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &start, &end, perms, &path_offset) < 3)
                continue;
            if (perms[2] != 'x' || count >= (int)(sizeof(g_ranges) / sizeof(*g_ranges))) continue;
            const char *path = path_offset ? line + path_offset : "";
            g_ranges[count].start = (uintptr_t)start;
            g_ranges[count].end = (uintptr_t)end;
            g_ranges[count].host = (uint8_t)mapping_is_host(path);
            ++count;
        }
    }
    if (fd >= 0) close(fd);
    atomic_store(&g_range_count, count);
    atomic_flag_clear_explicit(&g_ranges_lock, memory_order_release);
}

static int ranges_find(uintptr_t address)
{
    int count = atomic_load(&g_range_count);
    for (int i = 0; i < count; ++i)
        if (address >= g_ranges[i].start && address < g_ranges[i].end) return g_ranges[i].host;
    return -1;
}

static int address_is_host(uintptr_t address)
{
    int host = ranges_find(address);
    if (host < 0) {
        ranges_refresh();
        host = ranges_find(address);
    }
    return host == 1;
}

/* ---- Service dispatch ---- */
static void trace_object_name(const char *service, greg_t *regs, char *line, size_t size)
{
    uint64_t attributes = 0;
    if (!strcmp(service, "NtQueryAttributesFile") || !strcmp(service, "NtQueryFullAttributesFile"))
        attributes = (uint64_t)regs[REG_R10];
    else if (!strcmp(service, "NtCreateFile") || !strcmp(service, "NtOpenFile") ||
             !strcmp(service, "NtOpenSection"))
        attributes = (uint64_t)regs[REG_R8];
    if (!attributes) return;
    uint64_t object[3], unicode[2];
    uint16_t text[260];
    struct iovec local = { object, sizeof(object) };
    struct iovec remote = { (void *)(uintptr_t)attributes, sizeof(object) };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof(object)) return;
    local = (struct iovec){ unicode, sizeof(unicode) };
    remote = (struct iovec){ (void *)(uintptr_t)object[2], sizeof(unicode) };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof(unicode)) return;
    size_t chars = (unicode[0] & 0xffff) / 2;
    if (chars > 259) chars = 259;
    local = (struct iovec){ text, chars * 2 };
    remote = (struct iovec){ (void *)(uintptr_t)unicode[1], chars * 2 };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)(chars * 2)) return;
    char narrow[264];
    for (size_t i = 0; i < chars; ++i) narrow[i] = text[i] < 0x80 ? (char)text[i] : '?';
    narrow[chars] = 0;
    int length = snprintf(line, size, "[SYSCALL]   object \"%s\"\n", narrow);
    if (length > 0) (void)!write(2, line, (size_t)length);
}

static void beer_win32_syscall_signal(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    beer_win32_syscall_selector = 0;        /* host code below may make system calls */
    ucontext_t *uc = (ucontext_t *)context;
    greg_t *regs = uc->uc_mcontext.gregs;
    uint64_t rip = (uint64_t)regs[REG_RIP];

    /* Host code reached without the import trampoline (a COM method, a signal
     * handler) while guest calls were blocked: allow and run the call again. */
    if (address_is_host((uintptr_t)rip)) {
        regs[REG_RAX] = (greg_t)(unsigned)info->si_syscall;
        regs[REG_RIP] = (greg_t)(rip - 2);
        return;
    }

    uint64_t status = BEER_STATUS_INVALID_SYSTEM_SERVICE;
    unsigned number = (unsigned)info->si_syscall;
    BeerWin32Service *service =
        number < BEER_WIN32_SYSCALL_CAPACITY && g_services[number].name[0] ? &g_services[number] : NULL;

    /* Stack arguments five onwards follow the return address and the 32-byte
     * home area. Read them without trusting the guest RSP. */
    uint64_t stack[8] = {0};
    struct iovec local = { stack, sizeof(stack) };
    struct iovec remote = { (void *)(uintptr_t)(regs[REG_RSP] + 0x28), sizeof(stack) };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)sizeof(stack))
        memset(stack, 0, sizeof(stack));

    if (service && g_continue && !strcmp(service->name, "NtContinue")) {
        const uint64_t context = (uint64_t)regs[REG_R10];
        status = g_continue(uc, context, (uint64_t)regs[REG_RDX]);
        if (g_trace < 0) g_trace = getenv("BEER_API_TRACE") != NULL || getenv("BEER_SYSCALL_DIAGNOSTICS") != NULL;
        if (g_trace) {
            char line[160];
            int length = snprintf(line, sizeof(line), "[SYSCALL] 0x%x NtContinue(%llx) rip=%llx -> 0x%x, resume rip=%llx rsp=%llx\n",
                                  number, (unsigned long long)context, (unsigned long long)rip, (unsigned)status,
                                  (unsigned long long)regs[REG_RIP], (unsigned long long)regs[REG_RSP]);
            if (length > 0) (void)!write(2, line, (size_t)length);
        }
        if (status == 0) {                  /* the frame now holds the CONTEXT's registers */
            beer_win32_syscall_resume_rip = (uint64_t)regs[REG_RIP];
            regs[REG_RIP] = (greg_t)(uintptr_t)beer_win32_syscall_resume;
            return;
        }
    } else if (service && service->fn) {
        status = (uint32_t)service->fn((uint64_t)regs[REG_R10], (uint64_t)regs[REG_RDX],
                                       (uint64_t)regs[REG_R8], (uint64_t)regs[REG_R9],
                                       stack[0], stack[1], stack[2], stack[3],
                                       stack[4], stack[5], stack[6], stack[7]);
    } else if (service) {
        status = BEER_STATUS_NOT_IMPLEMENTED;
    }

    if (g_trace < 0) g_trace = getenv("BEER_API_TRACE") != NULL || getenv("BEER_SYSCALL_DIAGNOSTICS") != NULL;
    if (g_trace || !service || !service->fn) {
        static _Atomic uint8_t invalid_reported[BEER_WIN32_SYSCALL_CAPACITY];
        int first = service ? !service->reported
                            : number >= BEER_WIN32_SYSCALL_CAPACITY || !atomic_exchange(&invalid_reported[number], 1);
        if (g_trace || first) {
            char line[256];
            int length = snprintf(line, sizeof(line),
                                  "[SYSCALL] 0x%x %s(%llx, %llx, %llx, %llx) rip=%llx -> 0x%llx%s\n",
                                  number, service ? service->name : "<no Beer service>",
                                  (unsigned long long)regs[REG_R10], (unsigned long long)regs[REG_RDX],
                                  (unsigned long long)regs[REG_R8], (unsigned long long)regs[REG_R9],
                                  (unsigned long long)rip, (unsigned long long)status,
                                  service && !service->fn ? " (not implemented)" : "");
            if (length > 0) (void)!write(2, line, (size_t)length);
            if (service) {
                service->reported = 1;
                trace_object_name(service->name, regs, line, sizeof(line));
            }
        }
    }
    regs[REG_RAX] = (greg_t)status;
    beer_win32_syscall_resume_rip = rip;
    regs[REG_RIP] = (greg_t)(uintptr_t)beer_win32_syscall_resume;
}

void beer_win32_syscall_thread_init(void)
{
    if (!g_installed) return;
    beer_win32_syscall_selector = 0;
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, 0, 0,
              &beer_win32_syscall_selector) != 0) {
        static int reported;
        if (!reported++) perror("[SYSCALL] PR_SET_SYSCALL_USER_DISPATCH");
    }
}

int beer_win32_syscall_active(void) { return g_installed; }

int beer_win32_syscall_install(void)
{
    ssize_t length = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path) - 1);
    g_exe_path[length > 0 ? length : 0] = 0;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = beer_win32_syscall_signal;
    /* A Beer implementation can run guest code (for example a callback), and
     * that code may make another system call, so SIGSYS stays deliverable
     * while the handler runs. */
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    if (sigaction(SIGSYS, &action, NULL) != 0) return 0;

    beer_win32_syscall_selector = 0;
    if (prctl(PR_SET_SYSCALL_USER_DISPATCH, PR_SYS_DISPATCH_ON, 0, 0,
              &beer_win32_syscall_selector) != 0)
        return 0;
    g_installed = 1;
    ranges_refresh();
    return 1;
}
