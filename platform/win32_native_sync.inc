/* Native synchronization uses the same objects as Kernel32. Unsupported
 * namespace/security policies fail explicitly, rather than creating an
 * anonymous object in place of a requested named or secured one.
 * Contract: Microsoft Learn ZwCreateEvent/ZwSetEvent. */
typedef struct {
    u32 Length, Padding;
    u64 RootDirectory, ObjectName;
    u32 Attributes, Padding2;
    u64 SecurityDescriptor, SecurityQualityOfService;
} BeerSyncObjectAttributes;
_Static_assert(sizeof(BeerSyncObjectAttributes) == 48, "OBJECT_ATTRIBUTES Win64");

static u32 native_sync_attributes(const BeerSyncObjectAttributes *attributes)
{
    if (!attributes) return NT_STATUS_SUCCESS;
    if (getenv("BEER_THREAD_DIAGNOSTICS")) {
        fprintf(stderr, "[NTOBJECT] length=%u root=0x%llx name=0x%llx flags=0x%x security=0x%llx qos=0x%llx\n",
                attributes->Length, (unsigned long long)attributes->RootDirectory,
                (unsigned long long)attributes->ObjectName, attributes->Attributes,
                (unsigned long long)attributes->SecurityDescriptor,
                (unsigned long long)attributes->SecurityQualityOfService);
    }
    if (attributes->Length != sizeof(*attributes)) return NT_STATUS_INVALID_PARAMETER;
    if (attributes->RootDirectory || attributes->ObjectName || attributes->Attributes ||
        attributes->SecurityDescriptor || attributes->SecurityQualityOfService)
        return BEER_STATUS_NOT_IMPLEMENTED;
    return NT_STATUS_SUCCESS;
}

static u64 __attribute__((ms_abi))
impl_NtCreateEvent(u64 *handle, u32 access, const BeerSyncObjectAttributes *attributes,
                   u32 type, u8 initial)
{
    if (!handle) return NT_STATUS_ACCESS_VIOLATION;
    if (type > 1) return 0xC00000F2u; /* STATUS_INVALID_PARAMETER_4 */
    u32 status = native_sync_attributes(attributes);
    if (status) return status;
    /* Handle-specific restricted rights are not yet tracked by this backend. */
    if (access != 0x1f0003u && access != 0x10000000u) return BEER_STATUS_NOT_IMPLEMENTED;
    u64 event = event_alloc(type == 0, initial != 0);
    if (!event) return 0xC000009Au; /* STATUS_INSUFFICIENT_RESOURCES */
    *handle = event;
    return NT_STATUS_SUCCESS;
}

static u64 __attribute__((ms_abi)) impl_NtSetEvent(u64 handle, s32 *previous)
{
    return event_set_with_previous(handle, previous) ? NT_STATUS_SUCCESS : NT_STATUS_INVALID_HANDLE;
}

static u64 __attribute__((ms_abi)) impl_NtResetEvent(u64 handle, s32 *previous)
{
    int id = event_id_from_handle(handle);
    if (id < 0) return NT_STATUS_INVALID_HANDLE;
    WinEvent *event = &g_events[id];
    pthread_mutex_lock(&event->mtx);
    if (previous) *previous = event->signaled != 0;
    event->signaled = 0;
    pthread_mutex_unlock(&event->mtx);
    return NT_STATUS_SUCCESS;
}

static u64 __attribute__((ms_abi)) impl_NtClearEvent(u64 handle)
{
    return impl_NtResetEvent(handle, NULL);
}

/* Current-process threads use the established guest thread-entry backend.
 * Native CREATE_SUSPENDED is bit 0, unlike Kernel32's bit 2. Extended
 * attributes, alternate stack layouts, and additional flags are not ignored. */
static u64 __attribute__((ms_abi))
impl_NtCreateThreadEx(u64 *handle, u32 access, const BeerSyncObjectAttributes *attributes,
                      u64 process, u64 start, u64 argument, u32 flags, u64 zero_bits,
                      u64 stack_size, u64 maximum_stack_size, const void *attribute_list)
{
    if (getenv("BEER_THREAD_DIAGNOSTICS"))
        fprintf(stderr, "[NTTHREAD] start=0x%llx argument=0x%llx flags=0x%x zero_bits=%llu stack=%llu maximum=%llu attributes=%p\n",
                (unsigned long long)start, (unsigned long long)argument, flags,
                (unsigned long long)zero_bits, (unsigned long long)stack_size,
                (unsigned long long)maximum_stack_size, attribute_list);
    if (attribute_list && getenv("BEER_THREAD_DIAGNOSTICS")) {
        u64 values[5];
        if (diag_peek_u64((u64)(uintptr_t)attribute_list, values, 5))
            fprintf(stderr, "[NTTHREAD] attribute-list size=%llu first={0x%llx,%llu,0x%llx,0x%llx}\n",
                    (unsigned long long)values[0], (unsigned long long)values[1],
                    (unsigned long long)values[2], (unsigned long long)values[3],
                    (unsigned long long)values[4]);
    }
    if (!handle) return NT_STATUS_ACCESS_VIOLATION;
    if (!native_is_current_process(process)) return NT_STATUS_INVALID_HANDLE;
    if (!start) return NT_STATUS_INVALID_PARAMETER;
    u32 status = native_sync_attributes(attributes);
    if (status) return status;
    if (access != 0x1fffffu && access != 0x10000000u) return BEER_STATUS_NOT_IMPLEMENTED;
    if ((flags & ~1u) || zero_bits || stack_size || maximum_stack_size || attribute_list)
        return BEER_STATUS_NOT_IMPLEMENTED;
    u64 thread = impl_CreateThread(0, 0, start, argument, (flags & 1) ? 4 : 0, NULL);
    if (!thread) return 0xC000009Au; /* STATUS_INSUFFICIENT_RESOURCES */
    *handle = thread;
    return NT_STATUS_SUCCESS;
}