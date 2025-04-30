#include "BandwidthThrottler.h"

// --- Global Variable Definitions ---
HANDLE g_WfpEngineHandle = NULL; [cite: 33]
PDEVICE_OBJECT g_DeviceObject = NULL; [cite: 34]
LIST_ENTRY g_RuleList; [cite: 35]
KSPIN_LOCK g_RuleListLock; [cite: 35]
KTIMER g_RefillTimer; [cite: 35]
KDPC g_RefillDpc; [cite: 35]
BOOLEAN g_DriverUnloading = FALSE; [cite: 35]
UINT32 g_CalloutIdALEConnectV4 = 0; [cite: 35]
UINT32 g_CalloutIdALEConnectV6 = 0; [cite: 35]
UINT32 g_CalloutIdTransportV4 = 0; [cite: 36]
UINT32 g_CalloutIdTransportV6 = 0; [cite: 36]

const UINT64 TIMER_INTERVAL_MILLIS = 100; // Refill interval [cite: 36]

// --- Driver Entry and Unload ---

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING ntDeviceName; [cite: 37]
    UNICODE_STRING dosDeviceName; [cite: 38]

    UNREFERENCED_PARAMETER(RegistryPath);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: DriverEntry\n"));

    g_DriverUnloading = FALSE; [cite: 38]
    InitializeListHead(&g_RuleList); [cite: 38]
    KeInitializeSpinLock(&g_RuleListLock); [cite: 38]

    // Create the device object for IOCTLs [cite: 39]
    RtlInitUnicodeString(&ntDeviceName, NT_DEVICE_NAME); [cite: 39]
    status = IoCreateDevice(
        DriverObject,
        0,                      // No device extension [cite: 40]
        &ntDeviceName,
        FILE_DEVICE_NETWORK,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_DeviceObject); [cite: 39]

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: Failed to create device object (0x%X)\n", status)); [cite: 40]
        return status; [cite: 40]
    }

    // Set dispatch routines [cite: 41]
    DriverObject->DriverUnload = DriverUnload; [cite: 41]
    DriverObject->MajorFunction[IRP_MJ_CREATE] =
    DriverObject->MajorFunction[IRP_MJ_CLOSE] =
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControl; [cite: 41] // Using same function [cite: 42]

    // Create a symbolic link for user-mode access [cite: 42]
    RtlInitUnicodeString(&dosDeviceName, DOS_DEVICE_NAME); [cite: 42]
    status = IoCreateSymbolicLink(&dosDeviceName, &ntDeviceName); [cite: 42]
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: Failed to create symbolic link (0x%X)\n", status)); [cite: 43]
        IoDeleteDevice(g_DeviceObject); [cite: 43]
        g_DeviceObject = NULL; [cite: 43]
        return status; [cite: 44]
    }

    // Initialize WFP [cite: 44]
    status = WfpRegisterCallouts(g_DeviceObject); [cite: 44]
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: WFP Registration failed (0x%X)\n", status)); [cite: 44]
        IoDeleteSymbolicLink(&dosDeviceName); [cite: 45]
        IoDeleteDevice(g_DeviceObject); [cite: 45]
        g_DeviceObject = NULL; [cite: 45]
        return status; [cite: 45]
    }

    // Initialize and start the refill timer [cite: 45]
    KeInitializeTimer(&g_RefillTimer); [cite: 45]
    KeInitializeDpc(&g_RefillDpc, TimerDpcRoutine, NULL); [cite: 45]
    StartRefillTimer(); [cite: 45]

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Driver loaded successfully.\n")); [cite: 46]
    return status; [cite: 46]
}

VOID
DriverUnload(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    UNICODE_STRING dosDeviceName; [cite: 46]
    UNREFERENCED_PARAMETER(DriverObject); [cite: 50]

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: DriverUnload\n")); [cite: 48]

    g_DriverUnloading = TRUE; [cite: 48]

    // Stop the timer [cite: 48]
    StopRefillTimer(); [cite: 48]

    // Unregister from WFP [cite: 48]
    WfpUnregisterCallouts(); // This will wait for pending callouts [cite: 49]

    // Cleanup resources [cite: 49]
    RtlInitUnicodeString(&dosDeviceName, DOS_DEVICE_NAME); [cite: 49]
    IoDeleteSymbolicLink(&dosDeviceName); [cite: 49]

    // Cleanup rules [cite: 49]
    CleanupRules(); [cite: 49]

    // Delete the device object [cite: 50]
    if (g_DeviceObject) {
        IoDeleteDevice(g_DeviceObject); [cite: 50]
        g_DeviceObject = NULL; [cite: 50]
    }


    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Driver unloaded.\n")); [cite: 51]
}


// --- IOCTL Handling ---

NTSTATUS
DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION irpSp; [cite: 51]
    NTSTATUS status = STATUS_SUCCESS; [cite: 52]
    ULONG inputBufferLength = 0; [cite: 52]
    ULONG outputBufferLength = 0; [cite: 52]
    ULONG ioControlCode = 0; [cite: 52]
    PVOID ioBuffer = NULL; [cite: 52]

    UNREFERENCED_PARAMETER(DeviceObject); [cite: 53]

    irpSp = IoGetCurrentIrpStackLocation(Irp); [cite: 53]
    inputBufferLength = irpSp->Parameters.DeviceIoControl.InputBufferLength; [cite: 53]
    outputBufferLength = irpSp->Parameters.DeviceIoControl.OutputBufferLength; [cite: 53]
    ioControlCode = irpSp->Parameters.DeviceIoControl.IoControlCode; [cite: 53]
    ioBuffer = Irp->AssociatedIrp.SystemBuffer; // METHOD_BUFFERED uses SystemBuffer [cite: 54]

    // Check for unload early [cite: 54]
    if (g_DriverUnloading) {
        status = STATUS_DELETE_PENDING; [cite: 54]
        goto Exit; [cite: 54]
    }

    switch (irpSp->MajorFunction) { [cite: 55]
        case IRP_MJ_CREATE: [cite: 55]
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: IRP_MJ_CREATE\n")); [cite: 55]
            status = STATUS_SUCCESS; [cite: 55]
            break; [cite: 55]

        case IRP_MJ_CLOSE: [cite: 55]
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: IRP_MJ_CLOSE\n")); [cite: 56]
            status = STATUS_SUCCESS; [cite: 56]
            break; [cite: 56]

        case IRP_MJ_DEVICE_CONTROL: [cite: 56]
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: IRP_MJ_DEVICE_CONTROL (Code: 0x%X)\n", ioControlCode)); [cite: 56]

            switch (ioControlCode) { [cite: 56]
                case IOCTL_ADD_RULE: [cite: 56]
                    if (inputBufferLength >= sizeof(USER_RULE_DATA) && ioBuffer != NULL) { [cite: 56]
                        status = AddRule((PUSER_RULE_DATA)ioBuffer); [cite: 56]
                    } else {
                        status = STATUS_INVALID_PARAMETER; [cite: 56]
                    }
                    Irp->IoStatus.Information = 0; // No data back [cite: 56]
                    break; [cite: 56]

                case IOCTL_REMOVE_RULE: [cite: 56]
                    if (inputBufferLength >= sizeof(ULONG) && ioBuffer != NULL) { [cite: 56]
                        status = RemoveRule(*(PULONG)ioBuffer); [cite: 56]
                    } else {
                        status = STATUS_INVALID_PARAMETER; [cite: 56]
                    }
                    Irp->IoStatus.Information = 0; // No data back [cite: 56]
                    break; [cite: 56]

                // case IOCTL_QUERY_RULES: [cite: 57]
                    // TODO: Implement Query Rules - requires careful buffer management [cite: 57]
                    // status = QueryRules(ioBuffer, outputBufferLength, &Irp->IoStatus.Information); [cite: 57]
                    // break; [cite: 58]

                default: [cite: 58]
                    status = STATUS_INVALID_DEVICE_REQUEST; [cite: 58]
                    Irp->IoStatus.Information = 0; [cite: 58]
                    break; [cite: 58]
            }
            break; // End of IRP_MJ_DEVICE_CONTROL case [cite: 59]

        default: [cite: 59]
            status = STATUS_INVALID_DEVICE_REQUEST; [cite: 59]
            Irp->IoStatus.Information = 0; [cite: 59]
            break; [cite: 59]
    }

Exit: [cite: 58]
    Irp->IoStatus.Status = status; [cite: 59]
    Irp->IoStatus.Information = (status == STATUS_SUCCESS) ? Irp->IoStatus.Information : 0; // Keep info only on success (for Query)
    IoCompleteRequest(Irp, IO_NO_INCREMENT); [cite: 60]
    return status; [cite: 60]
}


// --- Rule Management ---

NTSTATUS AddRule(PUSER_RULE_DATA ruleData) {
    NTSTATUS status = STATUS_SUCCESS; [cite: 60]
    PTHROTTLE_RULE pRule = NULL; [cite: 61]
    PTHROTTLE_RULE pExistingRule = NULL; [cite: 61]
    KLOCK_QUEUE_HANDLE lockHandle; [cite: 61]

    if (ruleData == NULL || ruleData->ProcessId == 0) { [cite: 62]
        return STATUS_INVALID_PARAMETER; [cite: 62]
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: AddRule PID=%u, Up=%u KBps, Down=%u KBps\n", ruleData->ProcessId, ruleData->UploadLimitKBps, ruleData->DownloadLimitKBps)); [cite: 62]

    KeAcquireSpinLock(&g_RuleListLock, &lockHandle); [cite: 62]

    pExistingRule = FindRuleByPid(ruleData->ProcessId); // Find requires lock already held [cite: 62]

    if (pExistingRule) { [cite: 63]
        // Update existing rule's limits (re-initialize buckets) [cite: 63]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Updating existing rule for PID %u\n", ruleData->ProcessId)); [cite: 63]
        InitializeTokenBucket(&pExistingRule->UploadBucket, ruleData->UploadLimitKBps, TIMER_INTERVAL_MILLIS); [cite: 64]
        InitializeTokenBucket(&pExistingRule->DownloadBucket, ruleData->DownloadLimitKBps, TIMER_INTERVAL_MILLIS); [cite: 64]
        // Reset Active flag? Maybe not necessary, flows will re-evaluate on next packet [cite: 65]
    } else {
        // Allocate new rule structure [cite: 65]
        pRule = (PTHROTTLE_RULE)ExAllocatePoolZero(NonPagedPoolNx, sizeof(THROTTLE_RULE), POOL_TAG); [cite: 65]
        if (!pRule) { [cite: 66]
            status = STATUS_INSUFFICIENT_RESOURCES; [cite: 66]
            goto Exit; [cite: 66]
        }

        pRule->ProcessId = ruleData->ProcessId; [cite: 66]
        pRule->Active = FALSE; // Initially inactive until a flow uses it [cite: 67]
        pRule->ReferenceCount = 1; // Initial reference for being in the list [cite: 67]

        // Initialize token buckets [cite: 67]
        InitializeTokenBucket(&pRule->UploadBucket, ruleData->UploadLimitKBps, TIMER_INTERVAL_MILLIS); [cite: 67]
        InitializeTokenBucket(&pRule->DownloadBucket, ruleData->DownloadLimitKBps, TIMER_INTERVAL_MILLIS); [cite: 68]

        // Add to list [cite: 68]
        InsertTailList(&g_RuleList, &pRule->Link); [cite: 68]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Added new rule for PID %u\n", ruleData->ProcessId)); [cite: 68]
    }

Exit: [cite: 69]
    KeReleaseSpinLock(&g_RuleListLock, lockHandle); [cite: 69]
    return status; [cite: 69]
}

NTSTATUS RemoveRule(ULONG processId) {
    NTSTATUS status = STATUS_NOT_FOUND; [cite: 69]
    PTHROTTLE_RULE pRule = NULL; [cite: 69]
    PLIST_ENTRY pEntry; [cite: 70]
    KLOCK_QUEUE_HANDLE lockHandle; [cite: 70]

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: RemoveRule PID=%u\n", processId)); [cite: 70]
    if (processId == 0) { [cite: 71]
        return STATUS_INVALID_PARAMETER; [cite: 71]
    }

    KeAcquireSpinLock(&g_RuleListLock, &lockHandle); [cite: 71]

    for (pEntry = g_RuleList.Flink; pEntry != &g_RuleList; pEntry = pEntry->Flink) { [cite: 71]
        pRule = CONTAINING_RECORD(pEntry, THROTTLE_RULE, Link); [cite: 71]
        if (pRule->ProcessId == processId) { [cite: 71]
            RemoveEntryList(&pRule->Link); // Remove from list [cite: 71]
            status = STATUS_SUCCESS; [cite: 71]
            break; // Found and removed [cite: 71]
        }
        pRule = NULL; // Not the one [cite: 71]
    }

    KeReleaseSpinLock(&g_RuleListLock, lockHandle); [cite: 71]

    if (NT_SUCCESS(status) && pRule) { [cite: 71]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Removed rule for PID %u, dereferencing...\n", processId)); [cite: 71]
        // Dereference the rule (which might free it if refcount reaches zero) [cite: 71]
        DereferenceRule(pRule); [cite: 71]
    } else {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARN_LEVEL, "BandwidthThrottler: Rule for PID %u not found for removal.\n", processId)); [cite: 71]
    }

    return status; [cite: 71]
}

// Find rule by PID - MUST be called with g_RuleListLock held
_Requires_lock_held_(g_RuleListLock)
PTHROTTLE_RULE FindRuleByPid(ULONG processId) { [cite: 71]
    PLIST_ENTRY pEntry; [cite: 72]
    PTHROTTLE_RULE pRule = NULL; [cite: 72]

    if (processId == 0) return NULL; [cite: 72]

    for (pEntry = g_RuleList.Flink; pEntry != &g_RuleList; pEntry = pEntry->Flink) { [cite: 73]
        pRule = CONTAINING_RECORD(pEntry, THROTTLE_RULE, Link); [cite: 73]
        if (pRule->ProcessId == processId) { [cite: 74]
            return pRule; [cite: 74]
        }
    }
    return NULL; // Not found [cite: 74]
}


VOID InitializeTokenBucket(PTOKEN_BUCKET bucket, ULONG limitKBps, UINT64 timerIntervalMillis) { [cite: 74]
    KLOCK_QUEUE_HANDLE lockHandle; [cite: 74]
    UINT64 limitBytesPerSec = (UINT64)limitKBps * 1024; [cite: 75]

    KeInitializeSpinLock(&bucket->Lock); [cite: 75]
    KeAcquireSpinLock(&bucket->Lock, &lockHandle); [cite: 75]

    // Capacity: Allow bursts up to 1 second worth of data (adjust as needed) [cite: 76]
    bucket->CapacityBytes = limitBytesPerSec; [cite: 76]

    // Refill Rate: Bytes per timer interval [cite: 77]
    // Ensure no division by zero if interval is 0, though it shouldn't be.
    if (timerIntervalMillis > 0) {
         bucket->RefillRateBytesPerTick = (limitBytesPerSec * timerIntervalMillis) / 1000; [cite: 77]
    } else {
         bucket->RefillRateBytesPerTick = 0; // Or handle error
    }

    // Initial state: Full bucket [cite: 77]
    bucket->CurrentTokensBytes = bucket->CapacityBytes; [cite: 77]
    KeQuerySystemTimePrecise(&bucket->LastRefillTimestamp); // Use precise time [cite: 77]

    KeReleaseSpinLock(&bucket->Lock, lockHandle); [cite: 77]

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Init Bucket: Limit=%u KBps, Capacity=%llu bytes, RefillRate=%llu bytes/tick\n", limitKBps, bucket->CapacityBytes, bucket->RefillRateBytesPerTick)); [cite: 77]
}


// Returns TRUE if tokens were consumed, FALSE otherwise (block)
BOOLEAN ConsumeTokens(PTOKEN_BUCKET bucket, SIZE_T bytes) {
    KLOCK_QUEUE_HANDLE lockHandle; [cite: 78]
    BOOLEAN allowed = FALSE; [cite: 78]

    if (bucket->RefillRateBytesPerTick == 0 && bucket->CapacityBytes == 0) { // 0 KBps limit means block all [cite: 77]
         // Allow consuming initial burst capacity if limit is 0 but capacity > 0?
         // Current logic: if rate is 0, block everything after initial capacity is gone.
        KeAcquireSpinLock(&bucket->Lock, &lockHandle); [cite: 78]
        if(bucket->CurrentTokensBytes >= bytes) {
            bucket->CurrentTokensBytes -= bytes;
            allowed = TRUE;
        } else {
            allowed = FALSE; [cite: 78]
        }
        KeReleaseSpinLock(&bucket->Lock, &lockHandle); [cite: 80]
        return allowed;
    }

    KeAcquireSpinLock(&bucket->Lock, &lockHandle); [cite: 78]

    // Refill logic moved to timer DPC [cite: 78]

    if (bucket->CurrentTokensBytes >= bytes) { [cite: 78]
        bucket->CurrentTokensBytes -= bytes; // Corrected subtraction [cite: 78]
        allowed = TRUE; [cite: 78]
    } else {
        allowed = FALSE; [cite: 79]
        // KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "Throttler: Blocking, needed %zu, have %llu\n", bytes, bucket->CurrentTokensBytes)); [cite: 79]
    }

    KeReleaseSpinLock(&bucket->Lock, lockHandle); [cite: 80]
    return allowed; [cite: 80]
}


VOID CleanupRules() {
    PLIST_ENTRY pEntry, pNextEntry; [cite: 80]
    PTHROTTLE_RULE pRule; [cite: 80]
    KLOCK_QUEUE_HANDLE lockHandle; [cite: 80]

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Cleaning up rules...\n")); [cite: 81]

    KeAcquireSpinLock(&g_RuleListLock, &lockHandle); [cite: 81]

    // Iterate and remove all rules from the list [cite: 82]
    pEntry = g_RuleList.Flink; [cite: 82]
    while (pEntry != &g_RuleList) { [cite: 82]
        pNextEntry = pEntry->Flink; // Get next before removing current [cite: 83]
        pRule = CONTAINING_RECORD(pEntry, THROTTLE_RULE, Link); [cite: 83]

        RemoveEntryList(&pRule->Link); [cite: 83]
        InitializeListHead(&pRule->Link); // Avoid double remove issues if deref happens later [cite: 84]

        // Release the initial reference held by the list [cite: 84]
        LONG currentRef = InterlockedDecrement(&pRule->ReferenceCount); [cite: 84]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: Cleanup: Deref rule PID %u, new ref %ld\n", pRule->ProcessId, currentRef)); [cite: 85]

        if (currentRef == 0) { [cite: 86]
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: Cleanup: Freeing rule PID %u\n", pRule->ProcessId)); [cite: 86]
            ExFreePoolWithTag(pRule, POOL_TAG); [cite: 86]
        } else {
            // This case shouldn't normally happen during unload if FlowDelete is working, [cite: 87]
            // but indicates flows might still hold references. [cite: 87]
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARN_LEVEL, "BandwidthThrottler: Cleanup Rule PID %u still has %ld references after list removal!\n", pRule->ProcessId, currentRef)); [cite: 88]
        }
        pEntry = pNextEntry; // Move to the next entry [cite: 82]
    }

    InitializeListHead(&g_RuleList); // Ensure list is empty [cite: 89]

    KeReleaseSpinLock(&g_RuleListLock, lockHandle); [cite: 89]
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Rule cleanup complete.\n")); [cite: 89]
}


VOID DereferenceRule(PTHROTTLE_RULE rule) { [cite: 90]
    if (rule) { [cite: 90]
        LONG currentRef = InterlockedDecrement(&rule->ReferenceCount); [cite: 90]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: Dereferencing rule PID %u, new ref %ld\n", rule->ProcessId, currentRef)); [cite: 91]
        if (currentRef == 0) { [cite: 92]
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Freeing rule PID %u as ref count reached zero.\n", rule->ProcessId)); [cite: 92]
            ExFreePoolWithTag(rule, POOL_TAG); [cite: 92]
        }
    }
}


// --- Timer DPC ---

_Function_class_(KDEFERRED_ROUTINE)
VOID NTAPI TimerDpcRoutine(
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
)
{
    PLIST_ENTRY pEntry; [cite: 93]
    PTHROTTLE_RULE pRule; [cite: 93]
    KLOCK_QUEUE_HANDLE listLockHandle, bucketLockHandle; [cite: 94]
    LARGE_INTEGER currentTime; [cite: 94]
    UINT64 elapsedMillis; [cite: 94]
    UINT64 ticksElapsed; [cite: 94]
    UINT64 refillAmount; [cite: 94]

    UNREFERENCED_PARAMETER(Dpc); [cite: 94]
    UNREFERENCED_PARAMETER(DeferredContext); [cite: 95]
    UNREFERENCED_PARAMETER(SystemArgument1); [cite: 95]
    UNREFERENCED_PARAMETER(SystemArgument2); [cite: 95]

    // Protect against unload race condition [cite: 95]
    if (g_DriverUnloading) { [cite: 95]
        return; [cite: 95]
    }

    KeQuerySystemTimePrecise(&currentTime); // Use precise time [cite: 96]

    KeAcquireSpinLock(&g_RuleListLock, &listLockHandle); [cite: 96]

    for (pEntry = g_RuleList.Flink; pEntry != &g_RuleList; pEntry = pEntry->Flink) { [cite: 96]
        pRule = CONTAINING_RECORD(pEntry, THROTTLE_RULE, Link); [cite: 96]

        // Only refill buckets if the rule is actively used by flows? [cite: 97]
        // Or always refill? Let's always refill for simplicity now. [cite: 98]
        // if (pRule->Active) { [cite: 98]

            // Refill Upload Bucket [cite: 98]
            KeAcquireSpinLock(&pRule->UploadBucket.Lock, &bucketLockHandle); [cite: 98]
            if (pRule->UploadBucket.RefillRateBytesPerTick > 0) { // Only refill if there's a limit > 0 [cite: 99]
                // Calculate elapsed time in 100ns units, then convert to ms [cite: 99]
                elapsedMillis = (currentTime.QuadPart - pRule->UploadBucket.LastRefillTimestamp.QuadPart) / 10000; // 100ns units to ms [cite: 100]
                ticksElapsed = elapsedMillis / TIMER_INTERVAL_MILLIS; [cite: 100]

                if (ticksElapsed > 0) { [cite: 100]
                    refillAmount = ticksElapsed * pRule->UploadBucket.RefillRateBytesPerTick; [cite: 100]
                    pRule->UploadBucket.CurrentTokensBytes += refillAmount; [cite: 100]

                    if (pRule->UploadBucket.CurrentTokensBytes > pRule->UploadBucket.CapacityBytes) { [cite: 100]
                        pRule->UploadBucket.CurrentTokensBytes = pRule->UploadBucket.CapacityBytes; [cite: 100]
                    }
                    // Update timestamp only if we actually processed ticks to avoid drift [cite: 101]
                    pRule->UploadBucket.LastRefillTimestamp.QuadPart += ticksElapsed * TIMER_INTERVAL_MILLIS * 10000; // ms back to 100ns units [cite: 101]
                }
            }
            KeReleaseSpinLock(&pRule->UploadBucket.Lock, bucketLockHandle); [cite: 101]


            // Refill Download Bucket [cite: 102]
            KeAcquireSpinLock(&pRule->DownloadBucket.Lock, &bucketLockHandle); [cite: 102]
             if (pRule->DownloadBucket.RefillRateBytesPerTick > 0) { [cite: 103]
                 elapsedMillis = (currentTime.QuadPart - pRule->DownloadBucket.LastRefillTimestamp.QuadPart) / 10000; // 100ns units to ms [cite: 103]
                 ticksElapsed = elapsedMillis / TIMER_INTERVAL_MILLIS; [cite: 103]

                 if (ticksElapsed > 0) { [cite: 103]
                     refillAmount = ticksElapsed * pRule->DownloadBucket.RefillRateBytesPerTick; [cite: 104]
                     pRule->DownloadBucket.CurrentTokensBytes += refillAmount; [cite: 104]

                     if (pRule->DownloadBucket.CurrentTokensBytes > pRule->DownloadBucket.CapacityBytes) { [cite: 104]
                         pRule->DownloadBucket.CurrentTokensBytes = pRule->DownloadBucket.CapacityBytes; [cite: 104]
                     }
                      // Update timestamp only if we actually processed ticks [cite: 101]
                     pRule->DownloadBucket.LastRefillTimestamp.QuadPart += ticksElapsed * TIMER_INTERVAL_MILLIS * 10000; // ms back to 100ns units [cite: 104]
                 }
             }
             KeReleaseSpinLock(&pRule->DownloadBucket.Lock, bucketLockHandle); [cite: 104]

        // } // End if pRule->Active [cite: 104]
    }

    KeReleaseSpinLock(&g_RuleListLock, listLockHandle); [cite: 104]

    // Reschedule the timer if not unloading [cite: 104]
    if (!g_DriverUnloading) { [cite: 104]
        StartRefillTimer(); // Restart the periodic timer [cite: 104]
    }
}

VOID StartRefillTimer() {
    LARGE_INTEGER dueTime; [cite: 104]
    // Negative value indicates relative time from now [cite: 104]
    dueTime.QuadPart = -((LONGLONG)TIMER_INTERVAL_MILLIS * 10 * 1000); // ms -> 100ns units [cite: 104]
    KeSetTimer(&g_RefillTimer, dueTime, &g_RefillDpc); [cite: 105]
}

VOID StopRefillTimer() {
    KeCancelTimer(&g_RefillTimer); [cite: 105]
    // Ensure DPC completes if it's running [cite: 105]
    KeFlushQueuedDpcs(); [cite: 106]
}


// --- WFP Registration ---

NTSTATUS WfpRegisterCallouts(PDEVICE_OBJECT deviceObject) {
    NTSTATUS status = STATUS_SUCCESS; [cite: 106]
    FWPM_SUBLAYER subLayer; [cite: 106]
    FWPS_CALLOUT calloutALE, calloutTransport; // Re-use struct for V4/V6 registration [cite: 107]
    FWPM_CALLOUT calloutAddALE, calloutAddTransport; [cite: 107]
    FWPM_FILTER filterALE, filterTransport; [cite: 107]
    // FWPM_FILTER_CONDITION filterConditions[1]; // Not used in this version [cite: 107]
    BOOLEAN engineOpened = FALSE; [cite: 108]
    BOOLEAN inTransaction = FALSE; [cite: 108]
    GUID filterALEConnectV4Key = { 0 }; // Need keys to delete later
    GUID filterALEConnectV6Key = { 0 };
    GUID filterTransportOutV4Key = { 0 };
    GUID filterTransportInV4Key = { 0 };
    GUID filterTransportOutV6Key = { 0 };
    GUID filterTransportInV6Key = { 0 };


    // Session for WFP operations [cite: 109]
    status = FwpmEngineOpen(NULL, RPC_C_AUTHN_WINNT, NULL, NULL, &g_WfpEngineHandle); [cite: 109]
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: FwpmEngineOpen failed: 0x%X\n", status)); [cite: 110]
        goto Exit; [cite: 110]
    }
    engineOpened = TRUE; [cite: 110]

    status = FwpmTransactionBegin(g_WfpEngineHandle, 0); [cite: 111]
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: FwpmTransactionBegin failed: 0x%X\n", status)); [cite: 111]
        goto Exit; [cite: 111]
    }
    inTransaction = TRUE; [cite: 112]

    // 1. Add Sublayer [cite: 112]
    RtlZeroMemory(&subLayer, sizeof(FWPM_SUBLAYER)); [cite: 112]
    subLayer.subLayerKey = BANDWIDTH_THROTTLER_SUBLAYER_GUID; // Use generated GUID [cite: 112]
    subLayer.displayData.name = L"Bandwidth Throttler Sublayer"; [cite: 112]
    subLayer.displayData.description = L"Sublayer for Bandwidth Throttler Filters"; [cite: 113]
    subLayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT; // Keep it after reboot [cite: 113]
    subLayer.weight = FWP_EMPTY; // Use default weight [cite: 114]
    status = FwpmSubLayerAdd(g_WfpEngineHandle, &subLayer, NULL); [cite: 114]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 114]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: FwpmSubLayerAdd failed: 0x%X\n", status)); [cite: 114]
        goto Exit; [cite: 115]
    }
    status = STATUS_SUCCESS; // Ignore already exists [cite: 115]


    // 2. Register Callouts (Kernel Mode Objects) [cite: 115]

    // Common properties [cite: 115]
    RtlZeroMemory(&calloutALE, sizeof(FWPS_CALLOUT)); [cite: 116]
    calloutALE.notifyFn = CalloutNotify; [cite: 116]
    calloutALE.flowDeleteFn = CalloutFlowDelete; // Important for cleanup [cite: 116]
    calloutALE.flags = FWPS_CALLOUT_FLAG_CONDITIONAL_ON_FLOW | // Only called if flow context exists (applies to transport) [cite: 117]
                       FWPS_CALLOUT_FLAG_ALLOW_OFFLOAD;        // Important for performance [cite: 117]

    RtlZeroMemory(&calloutTransport, sizeof(FWPS_CALLOUT)); [cite: 117]
    calloutTransport.notifyFn = CalloutNotify; [cite: 118]
    calloutTransport.flowDeleteFn = CalloutFlowDelete; [cite: 118]
    calloutTransport.flags = FWPS_CALLOUT_FLAG_CONDITIONAL_ON_FLOW | [cite: 118]
                           FWPS_CALLOUT_FLAG_ALLOW_OFFLOAD; [cite: 118]


    // 2a. ALE Connect/Flow Established Callout (V4 & V6) - For PID association [cite: 119]
    calloutALE.classifyFn = CalloutClassifyALEConnect; [cite: 119]

    // V4 [cite: 119]
    calloutALE.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V4; // Use generated GUID [cite: 119]
    status = FwpsCalloutRegister(deviceObject, &calloutALE, &g_CalloutIdALEConnectV4); [cite: 120]
    if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ALE V4 FwpsCalloutRegister failed: 0x%X\n", status)); goto Exit; } [cite: 120]

    // V6 [cite: 120]
    calloutALE.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V6; // Use generated GUID [cite: 120]
    status = FwpsCalloutRegister(deviceObject, &calloutALE, &g_CalloutIdALEConnectV6); [cite: 120]
    if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ALE V6 FwpsCalloutRegister failed: 0x%X\n", status)); goto Exit; } [cite: 121]


    // 2b. Transport Callout (V4 & V6) - For actual throttling [cite: 122]
    calloutTransport.classifyFn = CalloutClassifyTransport; [cite: 122]

    // V4 [cite: 122]
    calloutTransport.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V4; // Use generated GUID [cite: 122]
    status = FwpsCalloutRegister(deviceObject, &calloutTransport, &g_CalloutIdTransportV4); [cite: 123]
    if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Transport V4 FwpsCalloutRegister failed: 0x%X\n", status)); goto Exit; } [cite: 124]

    // V6 [cite: 124]
    calloutTransport.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V6; // Use generated GUID [cite: 124]
    status = FwpsCalloutRegister(deviceObject, &calloutTransport, &g_CalloutIdTransportV6); [cite: 124]
    if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Transport V6 FwpsCalloutRegister failed: 0x%X\n", status)); goto Exit; } [cite: 126]


    // 3. Add Callouts to WFP Engine (User Mode Objects linking to Kernel Callouts) [cite: 126]

    // V4 ALE [cite: 128]
    RtlZeroMemory(&calloutAddALE, sizeof(FWPM_CALLOUT)); [cite: 126]
    calloutAddALE.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V4; // Use generated GUID [cite: 128]
    calloutAddALE.applicableLayer = FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4; // Target layer [cite: 127]
    calloutAddALE.displayData.name = L"Bandwidth Throttler ALE Connect Callout V4"; [cite: 127]
    calloutAddALE.displayData.description = L"Associates PID with flow context for throttling V4"; [cite: 127]
    calloutAddALE.flags = FWPM_CALLOUT_FLAG_CONDITIONAL_ON_FLOW | FWPM_CALLOUT_FLAG_ALLOW_OFFLOAD; [cite: 128]
    status = FwpmCalloutAdd(g_WfpEngineHandle, &calloutAddALE, NULL, NULL); [cite: 129]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 129]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ALE V4 FwpmCalloutAdd failed: 0x%X\n", status)); [cite: 129]
        goto Exit; [cite: 130]
    }
    status = STATUS_SUCCESS; // Ignore already exists [cite: 131]

    // V6 ALE [cite: 131]
    calloutAddALE.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V6; // Use generated GUID [cite: 131]
    calloutAddALE.applicableLayer = FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6; [cite: 131]
    calloutAddALE.displayData.name = L"Bandwidth Throttler ALE Connect Callout V6"; [cite: 131]
    calloutAddALE.displayData.description = L"Associates PID with flow context for throttling V6"; [cite: 132]
    status = FwpmCalloutAdd(g_WfpEngineHandle, &calloutAddALE, NULL, NULL); [cite: 132]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 133]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ALE V6 FwpmCalloutAdd failed: 0x%X\n", status)); goto Exit; [cite: 133]
    }
    status = STATUS_SUCCESS; // Ignore already exists [cite: 134]


    // V4 Transport (Outbound and Inbound) [cite: 135]
    RtlZeroMemory(&calloutAddTransport, sizeof(FWPM_CALLOUT)); [cite: 134]
    calloutAddTransport.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V4; // Use generated GUID [cite: 135]
    calloutAddTransport.displayData.name = L"Bandwidth Throttler Transport Callout V4"; [cite: 134]
    calloutAddTransport.displayData.description = L"Performs packet throttling V4"; [cite: 134]
    calloutAddTransport.flags = FWPM_CALLOUT_FLAG_CONDITIONAL_ON_FLOW | FWPM_CALLOUT_FLAG_ALLOW_OFFLOAD; [cite: 134]

    calloutAddTransport.applicableLayer = FWPM_LAYER_OUTBOUND_TRANSPORT_V4; [cite: 135]
    status = FwpmCalloutAdd(g_WfpEngineHandle, &calloutAddTransport, NULL, NULL); [cite: 136]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 136]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Out Transport V4 FwpmCalloutAdd failed: 0x%X\n", status)); [cite: 136]
        goto Exit; [cite: 137]
    }
    status = STATUS_SUCCESS; [cite: 137]

    calloutAddTransport.applicableLayer = FWPM_LAYER_INBOUND_TRANSPORT_V4; // Add for Inbound too [cite: 137]
    status = FwpmCalloutAdd(g_WfpEngineHandle, &calloutAddTransport, NULL, NULL); [cite: 137]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 138]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "In Transport V4 FwpmCalloutAdd failed: 0x%X\n", status)); goto Exit; [cite: 138]
    }
    status = STATUS_SUCCESS; [cite: 138]


    // V6 Transport (Outbound and Inbound) [cite: 139]
    calloutAddTransport.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V6; // Use generated GUID [cite: 139]
    calloutAddTransport.displayData.name = L"Bandwidth Throttler Transport Callout V6"; [cite: 139]
    calloutAddTransport.displayData.description = L"Performs packet throttling V6"; [cite: 139]

    calloutAddTransport.applicableLayer = FWPM_LAYER_OUTBOUND_TRANSPORT_V6; [cite: 139]
    status = FwpmCalloutAdd(g_WfpEngineHandle, &calloutAddTransport, NULL, NULL); [cite: 140]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 140]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Out Transport V6 FwpmCalloutAdd failed: 0x%X\n", status)); [cite: 140]
        goto Exit; [cite: 141]
    }
    status = STATUS_SUCCESS; [cite: 141]

    calloutAddTransport.applicableLayer = FWPM_LAYER_INBOUND_TRANSPORT_V6; // Add for Inbound too [cite: 141]
    status = FwpmCalloutAdd(g_WfpEngineHandle, &calloutAddTransport, NULL, NULL); [cite: 141]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 142]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "In Transport V6 FwpmCalloutAdd failed: 0x%X\n", status)); [cite: 142]
        goto Exit; [cite: 143]
    }
    status = STATUS_SUCCESS; [cite: 143]


    // 4. Add Filters to Invoke Callouts [cite: 143]
    // Filters tell WFP *when* to call our callouts. [cite: 143]

    // 4a. ALE Connect Filter (V4 & V6) - Match any flow to try associating PID [cite: 144]
    RtlZeroMemory(&filterALE, sizeof(FWPM_FILTER)); [cite: 144]
    filterALE.subLayerKey = BANDWIDTH_THROTTLER_SUBLAYER_GUID; // Our sublayer [cite: 145]
    filterALE.weight.type = FWP_UINT64; // Use explicit weight [cite: 146]
    filterALE.weight.uint64 = (UINT64)-1; // High weight, inspect first [cite: 146]
    filterALE.action.type = FWP_ACTION_CALLOUT_INSPECTION; // Don't block/permit here, just inspect [cite: 147]
    // No filter conditions needed, match all traffic at this layer

    // V4 Filter [cite: 147]
    filterALE.layerKey = FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4; [cite: 147]
    filterALE.displayData.name = L"Bandwidth Throttler ALE Connect Filter V4"; [cite: 145]
    filterALE.displayData.description = L"Triggers PID association for V4 flows"; [cite: 145]
    filterALE.action.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V4; // Use generated GUID [cite: 147]
    // Assign a GUID to the filter itself so we can delete it by key later
    status = UuidCreate(&filterALEConnectV4Key); // Requires Rpcrt4.lib - Alternative: predefine GUIDs for filters
    if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ALE V4 UuidCreate failed: 0x%X\n", status)); goto Exit; }
    filterALE.filterKey = filterALEConnectV4Key;
    status = FwpmFilterAdd(g_WfpEngineHandle, &filterALE, NULL, NULL); [cite: 148]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 148]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ALE V4 FwpmFilterAdd failed: 0x%X\n", status)); goto Exit; [cite: 149]
    }
    status = STATUS_SUCCESS; [cite: 149]

    // V6 Filter [cite: 150]
    filterALE.layerKey = FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6; [cite: 150]
    filterALE.action.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V6; // Use generated GUID [cite: 150]
    filterALE.displayData.name = L"Bandwidth Throttler ALE Connect Filter V6"; [cite: 150]
    filterALE.displayData.description = L"Triggers PID association for V6 flows"; [cite: 151]
    status = UuidCreate(&filterALEConnectV6Key);
    if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ALE V6 UuidCreate failed: 0x%X\n", status)); goto Exit; }
    filterALE.filterKey = filterALEConnectV6Key;
    status = FwpmFilterAdd(g_WfpEngineHandle, &filterALE, NULL, NULL); [cite: 151]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 152]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ALE V6 FwpmFilterAdd failed: 0x%X\n", status)); goto Exit; [cite: 152]
    }
    status = STATUS_SUCCESS; [cite: 152]


    // 4b. Transport Filter (V4 & V6) - Match flows *with our context* [cite: 153]
    RtlZeroMemory(&filterTransport, sizeof(FWPM_FILTER)); [cite: 153]
    filterTransport.subLayerKey = BANDWIDTH_THROTTLER_SUBLAYER_GUID; [cite: 153]
    filterTransport.weight.type = FWP_UINT64; [cite: 154]
    filterTransport.weight.uint64 = (UINT64)-2; // Lower weight than ALE, higher than default permit [cite: 155]
    filterTransport.action.type = FWP_ACTION_CALLOUT_INSPECTION; // Callout decides permit/block [cite: 155]
    filterTransport.numFilterConditions = 0; // Matches everything in the layer initially [cite: 156]
    filterTransport.filterCondition = NULL; [cite: 156]
    // IMPORTANT: Filter based on flow context existence! [cite: 157]
    // This ensures the Transport callout is *only* invoked if the ALE callout associated context. [cite: 157]
    filterTransport.flags = FWPM_FILTER_FLAG_HAS_PROVIDER_CONTEXT; // Only match flows with *any* provider context [cite: 158]
                                                                  // We check within the callout if it's *our* context. [cite: 158]

    // V4 Transport Filter (Outbound) [cite: 159]
    filterTransport.layerKey = FWPM_LAYER_OUTBOUND_TRANSPORT_V4; [cite: 159]
    filterTransport.displayData.name = L"Bandwidth Throttler Transport Filter Out V4"; // Specific name [cite: 154]
    filterTransport.displayData.description = L"Triggers throttling for associated V4 flows (Outbound)"; // Specific description [cite: 154]
    filterTransport.action.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V4; // Use generated GUID [cite: 159]
    status = UuidCreate(&filterTransportOutV4Key);
    if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Out Transport V4 UuidCreate failed: 0x%X\n", status)); goto Exit; }
    filterTransport.filterKey = filterTransportOutV4Key;
    status = FwpmFilterAdd(g_WfpEngineHandle, &filterTransport, NULL, NULL); [cite: 159]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 160]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Out Transport V4 FwpmFilterAdd failed: 0x%X\n", status)); goto Exit; [cite: 160]
    }
    status = STATUS_SUCCESS; [cite: 160]

    // V4 Transport Filter (Inbound) [cite: 161]
    filterTransport.layerKey = FWPM_LAYER_INBOUND_TRANSPORT_V4; [cite: 161]
    filterTransport.displayData.name = L"Bandwidth Throttler Transport Filter In V4"; // Specific name
    filterTransport.displayData.description = L"Triggers throttling for associated V4 flows (Inbound)"; // Specific description
    // action.calloutKey remains the same (BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V4)
    status = UuidCreate(&filterTransportInV4Key);
     if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "In Transport V4 UuidCreate failed: 0x%X\n", status)); goto Exit; }
    filterTransport.filterKey = filterTransportInV4Key;
    status = FwpmFilterAdd(g_WfpEngineHandle, &filterTransport, NULL, NULL); [cite: 161]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 162]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "In Transport V4 FwpmFilterAdd failed: 0x%X\n", status)); [cite: 162]
        goto Exit; [cite: 163]
    }
    status = STATUS_SUCCESS; [cite: 163]

    // V6 Transport Filter (Outbound) [cite: 163]
    filterTransport.layerKey = FWPM_LAYER_OUTBOUND_TRANSPORT_V6; [cite: 163]
    filterTransport.action.calloutKey = BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V6; // Use generated GUID [cite: 164]
    filterTransport.displayData.name = L"Bandwidth Throttler Transport Filter Out V6"; // Specific name [cite: 164]
    filterTransport.displayData.description = L"Triggers throttling for associated V6 flows (Outbound)"; // Specific description [cite: 164]
    status = UuidCreate(&filterTransportOutV6Key);
     if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Out Transport V6 UuidCreate failed: 0x%X\n", status)); goto Exit; }
    filterTransport.filterKey = filterTransportOutV6Key;
    status = FwpmFilterAdd(g_WfpEngineHandle, &filterTransport, NULL, NULL); [cite: 165]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 165]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "Out Transport V6 FwpmFilterAdd failed: 0x%X\n", status)); [cite: 165]
        goto Exit; [cite: 166]
    }
    status = STATUS_SUCCESS; [cite: 166]

    // V6 Transport Filter (Inbound) [cite: 166]
    filterTransport.layerKey = FWPM_LAYER_INBOUND_TRANSPORT_V6; [cite: 166]
    filterTransport.displayData.name = L"Bandwidth Throttler Transport Filter In V6"; // Specific name
    filterTransport.displayData.description = L"Triggers throttling for associated V6 flows (Inbound)"; // Specific description
    // action.calloutKey remains the same (BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V6)
    status = UuidCreate(&filterTransportInV6Key);
     if (!NT_SUCCESS(status)) { KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "In Transport V6 UuidCreate failed: 0x%X\n", status)); goto Exit; }
    filterTransport.filterKey = filterTransportInV6Key;
    status = FwpmFilterAdd(g_WfpEngineHandle, &filterTransport, NULL, NULL); [cite: 166]
    if (!NT_SUCCESS(status) && status != STATUS_FWP_ALREADY_EXISTS) { [cite: 167]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "In Transport V6 FwpmFilterAdd failed: 0x%X\n", status)); goto Exit; [cite: 167]
    }
    status = STATUS_SUCCESS; [cite: 168]


    // 5. Commit Transaction [cite: 168]
    status = FwpmTransactionCommit(g_WfpEngineHandle); [cite: 168]
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: FwpmTransactionCommit failed: 0x%X\n", status)); [cite: 168]
        // Error will be handled in Exit block [cite: 169]
    } else {
         inTransaction = FALSE; // Committed successfully [cite: 169]
         KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: WFP Callouts and Filters registered successfully.\n")); [cite: 169]
    }


Exit: [cite: 170]
    if (!NT_SUCCESS(status)) { [cite: 170]
        if (inTransaction) { [cite: 170]
            FwpmTransactionAbort(g_WfpEngineHandle); [cite: 170]
        }
        // Rollback: Unregister anything that was successfully registered before failure [cite: 170]
        // Call unregister even on partial success [cite: 170]
        // Use the filter keys generated earlier if available
        FwpmFilterDeleteByKey(g_WfpEngineHandle, &filterALEConnectV4Key);
        FwpmFilterDeleteByKey(g_WfpEngineHandle, &filterALEConnectV6Key);
        FwpmFilterDeleteByKey(g_WfpEngineHandle, &filterTransportOutV4Key);
        FwpmFilterDeleteByKey(g_WfpEngineHandle, &filterTransportInV4Key);
        FwpmFilterDeleteByKey(g_WfpEngineHandle, &filterTransportOutV6Key);
        FwpmFilterDeleteByKey(g_WfpEngineHandle, &filterTransportInV6Key);
        // Call the main unregister function for callouts etc.
         WfpUnregisterCallouts(); // Call unregister for the rest [cite: 171]
    }

     // Close engine handle only if registration fully failed here,
     // otherwise keep it open for DriverUnload->WfpUnregisterCallouts
    if (!NT_SUCCESS(status) && engineOpened && g_WfpEngineHandle) { [cite: 171]
         FwpmEngineClose(g_WfpEngineHandle); [cite: 171]
         g_WfpEngineHandle = NULL; [cite: 171]
     } else if (NT_SUCCESS(status)) {
        // If successful, engineOpened remains TRUE and g_WfpEngineHandle is valid
        // It will be closed in DriverUnload
     }


    return status; [cite: 171]
}


NTSTATUS WfpUnregisterCallouts() {
    NTSTATUS status = STATUS_SUCCESS; [cite: 171]

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Unregistering WFP objects...\n")); [cite: 171]

    if (g_WfpEngineHandle == NULL) { [cite: 171]
        return STATUS_INVALID_HANDLE; // Already unregistered or never registered [cite: 171]
    }

    // Must be called at IRQL = PASSIVE_LEVEL [cite: 171]
    PAGED_CODE(); [cite: 171]

    // Best effort cleanup - ignore errors if objects don't exist [cite: 171]

    // Remove Filters first (references callouts)
    // Need filter GUIDs if persistent. If filters were added with dynamic GUIDs,
    // need to enumerate and delete them, or delete by sublayer.
    // Assuming filters were added with specific GUIDs or are non-persistent and
    // deleted when sublayer is deleted/callout is removed.
    // Deleting by sublayer key is often cleaner if filters aren't explicitly persistent.
    // FwpmFilterDeleteByKey(g_WfpEngineHandle, &FILTER_ALE_V4_GUID); // Example if known filter keys were used [cite: 171]
    // FwpmFilterDeleteByKey(g_WfpEngineHandle, &FILTER_ALE_V6_GUID); [cite: 171]
    // FwpmFilterDeleteByKey(g_WfpEngineHandle, &FILTER_TRANSPORT_OUT_V4_GUID); [cite: 172]
    // FwpmFilterDeleteByKey(g_WfpEngineHandle, &FILTER_TRANSPORT_IN_V4_GUID); [cite: 172]
    // FwpmFilterDeleteByKey(g_WfpEngineHandle, &FILTER_TRANSPORT_OUT_V6_GUID); [cite: 173]
    // FwpmFilterDeleteByKey(g_WfpEngineHandle, &FILTER_TRANSPORT_IN_V6_GUID); [cite: 173]

    // Attempt to delete filters by sublayer key (may require enumeration if this fails)
    // Note: FwpmFilterDeleteByKey requires a specific filter key, not a sublayer key. [cite: 175]
    // Deleting filters by sublayer requires enumerating filters associated with the sublayer
    // and deleting them individually by their filter IDs or keys.
    // For simplicity here, we rely on the callout/sublayer removal to clean up non-persistent filters.
    // If filters *were* made persistent, explicit deletion by key is required.

    // Remove Callouts from WFP Engine (User Mode Objects) [cite: 177]
    FwpmCalloutDeleteByKey(g_WfpEngineHandle, &BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V4); [cite: 177]
    FwpmCalloutDeleteByKey(g_WfpEngineHandle, &BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V6); [cite: 178]
    FwpmCalloutDeleteByKey(g_WfpEngineHandle, &BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V4); [cite: 178]
    FwpmCalloutDeleteByKey(g_WfpEngineHandle, &BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V6); [cite: 178]

    // Unregister Callouts (Kernel Mode Objects) - Waits for pending calls [cite: 179]
    if (g_CalloutIdALEConnectV4 != 0) { [cite: 179]
        FwpsCalloutUnregisterById(g_CalloutIdALEConnectV4); [cite: 179]
        g_CalloutIdALEConnectV4 = 0; [cite: 179]
    }
    if (g_CalloutIdALEConnectV6 != 0) { [cite: 179]
        FwpsCalloutUnregisterById(g_CalloutIdALEConnectV6); [cite: 179]
         g_CalloutIdALEConnectV6 = 0; [cite: 179]
    }
    if (g_CalloutIdTransportV4 != 0) { [cite: 179]
        FwpsCalloutUnregisterById(g_CalloutIdTransportV4); [cite: 179]
         g_CalloutIdTransportV4 = 0; [cite: 179]
    }
    if (g_CalloutIdTransportV6 != 0) { [cite: 179]
        FwpsCalloutUnregisterById(g_CalloutIdTransportV6); [cite: 179]
         g_CalloutIdTransportV6 = 0; [cite: 179]
    }

    // Remove Sublayer (optional, can leave persistent ones) [cite: 179]
     // If the sublayer is persistent, uncomment this to remove it.
    // status = FwpmSubLayerDeleteByKey(g_WfpEngineHandle, &BANDWIDTH_THROTTLER_SUBLAYER_GUID); [cite: 179]
    // if (!NT_SUCCESS(status) && status != STATUS_FWP_SUBLAYER_NOT_FOUND) {
    //     KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARN_LEVEL, "BandwidthThrottler: FwpmSubLayerDeleteByKey failed: 0x%X\n", status));
    //     // Continue cleanup even if sublayer deletion fails
    // }

    // Close WFP Engine Handle [cite: 179]
    if (g_WfpEngineHandle) { [cite: 179]
        FwpmEngineClose(g_WfpEngineHandle); [cite: 179]
        g_WfpEngineHandle = NULL; [cite: 179]
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: WFP Unregistration complete.\n")); [cite: 180]
    return status; // Return overall status (likely SUCCESS unless engine close failed) [cite: 181]
}


// --- WFP Callout Implementations ---

// Called at FWPS_LAYER_ALE_FLOW_ESTABLISHED_V4 / V6 [cite: 181]
_Use_decl_annotations_
void NTAPI CalloutClassifyALEConnect(
    const FWPS_INCOMING_VALUES* inFixedValues,
    const FWPS_INCOMING_METADATA_VALUES* inMetaValues,
    void* layerData,
    const void* classifyContext,
    const FWPS_FILTER* filter,
    UINT64 flowContext, // Input flow context (usually 0 here) [cite: 181]
    FWPS_CLASSIFY_OUT* classifyOut
)
{
    NTSTATUS status = STATUS_SUCCESS; [cite: 182]
    ULONG processId = 0; [cite: 182]
    PTHROTTLE_RULE pRule = NULL; [cite: 182]
    PFLOW_CONTEXT pFlowContext = NULL; [cite: 182]
    KLOCK_QUEUE_HANDLE lockHandle; [cite: 182]
    BOOLEAN isV4 = (inFixedValues->layerId == FWPS_LAYER_ALE_FLOW_ESTABLISHED_V4); [cite: 182]

    UNREFERENCED_PARAMETER(layerData); [cite: 183]
    UNREFERENCED_PARAMETER(classifyContext); [cite: 183]
    UNREFERENCED_PARAMETER(flowContext); // We are creating context, not using existing one here [cite: 183]

    // Default action is PERMIT unless we fail critically
    classifyOut->actionType = FWP_ACTION_PERMIT; [cite: 184]

    // Don't associate during unload [cite: 183]
    if (g_DriverUnloading) { [cite: 183]
        return; [cite: 184]
    }

    // Get Process ID [cite: 184]
    const FWP_VALUE* pidValue = NULL; [cite: 184]
    if (isV4) { [cite: 184]
        pidValue = &inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_PROCESS_ID]; [cite: 184]
    } else {
        pidValue = &inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_PROCESS_ID]; [cite: 184]
    }

    if (pidValue && pidValue->type == FWP_UINT64) { // PID is UINT64 here [cite: 185]
        processId = (ULONG)pidValue->uint64; // Cast to ULONG for our rule matching [cite: 186]
    }

    if (processId == 0) { [cite: 186]
        // System process or unable to get PID, permit and don't associate context [cite: 186]
        return; // Action is already PERMIT [cite: 187]
    }

    // Check if a rule exists for this PID [cite: 187]
    KeAcquireSpinLock(&g_RuleListLock, &lockHandle); [cite: 187]
    pRule = FindRuleByPid(processId); [cite: 187]
    if (pRule) { [cite: 188]
        // Increment reference count because flow context will point to it [cite: 188]
        InterlockedIncrement(&pRule->ReferenceCount); [cite: 188]
        pRule->Active = TRUE; // Mark rule as active [cite: 189]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: ALE: Found rule for PID %u, ref now %ld\n", processId, pRule->ReferenceCount)); [cite: 189]
    }
    KeReleaseSpinLock(&g_RuleListLock, lockHandle); [cite: 189]


    if (pRule) { [cite: 190]
        // Rule found, create and associate flow context [cite: 190]
        pFlowContext = (PFLOW_CONTEXT)ExAllocatePoolZero(NonPagedPoolNx, sizeof(FLOW_CONTEXT), POOL_TAG); [cite: 190]
        if (pFlowContext) { [cite: 191]
            pFlowContext->Rule = pRule; // Store pointer to the rule [cite: 191]

            // Determine direction (needed for transport layer) [cite: 191]
            // We can check FWPS_FIELD_ALE_FLOW_ESTABLISHED_V*_DIRECTION [cite: 191]
            const FWP_VALUE* directionValue = NULL; [cite: 191]
            if (isV4) { [cite: 192]
                directionValue = &inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_DIRECTION]; [cite: 192]
            } else {
                directionValue = &inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_DIRECTION]; [cite: 193]
            }

            if (directionValue && directionValue->type == FWP_UINT32 && directionValue->uint32 == FWP_DIRECTION_OUTBOUND) { [cite: 193]
                pFlowContext->IsUpload = TRUE; [cite: 193]
            } else {
                pFlowContext->IsUpload = FALSE; // Inbound or unspecified treated as download [cite: 194]
            }

            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: ALE: Associating context for PID %u, Flow %llu, Upload=%d\n", processId, inMetaValues->flowHandle, pFlowContext->IsUpload)); [cite: 194]

            // Associate context with the flow [cite: 195]
            // Use the specific callout ID from the filter that triggered this classify
            status = FwpsFlowAssociateContext(inMetaValues->flowHandle, filter->layerId, filter->action.calloutId, (UINT64)pFlowContext); [cite: 195]

            if (!NT_SUCCESS(status)) { [cite: 196]
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: ALE: FwpsFlowAssociateContext failed: 0x%X\n", status)); [cite: 196]
                // Failed to associate, free context and dereference rule we just added ref for [cite: 197]
                ExFreePoolWithTag(pFlowContext, POOL_TAG); [cite: 197]
                DereferenceRule(pRule); [cite: 197]
            } else {
                // Association successful, context ownership transferred to WFP. [cite: 198]
                // FlowDeleteFn will be called to free it later. [cite: 199]
                // pFlowContext and pRule are now managed by WFP/FlowDeleteFn
            }
        } else {
             KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "BandwidthThrottler: ALE: Failed to allocate flow context memory\n")); [cite: 199]
             DereferenceRule(pRule); // Dereference rule as we couldn't create context [cite: 200]
        }
    }

    // Always permit at ALE layer; decisions happen at Transport layer based on context [cite: 201]
    // Action is already PERMIT
}


// Called at FWPS_LAYER_INBOUND_TRANSPORT_V4/V6 and FWPS_LAYER_OUTBOUND_TRANSPORT_V4/V6 [cite: 202]
_Use_decl_annotations_
void NTAPI CalloutClassifyTransport(
    const FWPS_INCOMING_VALUES* inFixedValues,
    const FWPS_INCOMING_METADATA_VALUES* inMetaValues,
    void* layerData, // This is NET_BUFFER_LIST for Transport layers [cite: 202]
    const void* classifyContext,
    const FWPS_FILTER* filter,
    UINT64 flowContext, // Context associated by ALE callout [cite: 202]
    FWPS_CLASSIFY_OUT* classifyOut
)
{
    PFLOW_CONTEXT pFlowContext = (PFLOW_CONTEXT)flowContext; // Cast the UINT64 context [cite: 203]
    PNET_BUFFER_LIST netBufferList = (PNET_BUFFER_LIST)layerData; [cite: 203]
    SIZE_T packetSize = 0; [cite: 203]
    BOOLEAN allowed = TRUE; // Default to permit unless throttled [cite: 203]

    UNREFERENCED_PARAMETER(inFixedValues); [cite: 204]
    UNREFERENCED_PARAMETER(classifyContext); [cite: 204]
    UNREFERENCED_PARAMETER(filter); [cite: 204]
    UNREFERENCED_PARAMETER(inMetaValues); // Potentially useful for compartment ID etc.

    // If no flow context or driver unloading, permit [cite: 205]
    if (pFlowContext == NULL || pFlowContext->Rule == NULL || g_DriverUnloading) { [cite: 205]
        // Check rights before modifying (though default is often PERMIT)
         if (classifyOut->rights & FWPS_RIGHT_ACTION_WRITE) { [cite: 205]
            classifyOut->actionType = FWP_ACTION_PERMIT; [cite: 206]
         } else {
             KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARN_LEVEL, "BandwidthThrottler: Transport: No write right (context null/unload)!\n")); [cite: 205]
         }
        return; [cite: 206]
    }

    // Check if we have rights to modify action [cite: 206]
    if (!(classifyOut->rights & FWPS_RIGHT_ACTION_WRITE)) { [cite: 206]
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARN_LEVEL, "BandwidthThrottler: Transport: No write right, cannot enforce throttle!\n")); [cite: 206]
        // We can't block, maybe just continue? Or assert? For now, permit. [cite: 207]
        // Action Type is likely already PERMIT if we can't write, but explicitly set if possible.
        return; // Can't change the outcome
    }


    // Get packet size from NetBufferList [cite: 208]
    if (netBufferList) { [cite: 209]
        // Iterate through NBs if necessary, though NBL size often available
         packetSize = NET_BUFFER_LIST_CONTEXT_DATA_SIZE(netBufferList); // Check if this gives header+data or just data
         // More reliable way: iterate NBs
         PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(netBufferList); [cite: 210]
         packetSize = 0; // Reset packet size
         while (nb != NULL) { [cite: 211]
             packetSize += NET_BUFFER_DATA_LENGTH(nb); [cite: 211]
             nb = NET_BUFFER_NEXT_NB(nb); [cite: 211]
         }
    }


    if (packetSize == 0) { [cite: 212]
        classifyOut->actionType = FWP_ACTION_PERMIT; // Permit zero-length packets [cite: 212]
        return; [cite: 212]
    }

    // Consume tokens based on direction stored in context [cite: 212]
    if (pFlowContext->IsUpload) { // Outbound traffic [cite: 212]
        allowed = ConsumeTokens(&pFlowContext->Rule->UploadBucket, packetSize); [cite: 212]
    } else { // Inbound traffic [cite: 213]
        allowed = ConsumeTokens(&pFlowContext->Rule->DownloadBucket, packetSize); [cite: 213]
    }

    // Set action based on token availability [cite: 213]
    if (allowed) { [cite: 213]
        classifyOut->actionType = FWP_ACTION_PERMIT; [cite: 213]
    } else {
        classifyOut->actionType = FWP_ACTION_BLOCK; [cite: 213]
        classifyOut->flags |= FWPS_CLASSIFY_OUT_FLAG_ABSORB; // Prevent auditing/logging if desired [cite: 213]
        // KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: Transport: Blocking %zu bytes for PID %u, Upload=%d\n", packetSize, pFlowContext->Rule->ProcessId, pFlowContext->IsUpload)); [cite: 213]
    }
}


// Callout Notify Function (Optional but good practice) [cite: 213]
_Use_decl_annotations_
NTSTATUS NTAPI CalloutNotify(
    FWPS_CALLOUT_NOTIFY_TYPE notifyType,
    const GUID* filterKey,
    FWPS_FILTER* filter // Made pointer const as per function signature
)
{
    UNREFERENCED_PARAMETER(filterKey); [cite: 213]

    switch (notifyType) { [cite: 214]
        case FWPS_CALLOUT_NOTIFY_ADD_FILTER: [cite: 214]
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Filter added (Callout ID: %u, Filter ID: %llu)\n", filter->action.calloutId, filter->filterId)); [cite: 214]
            break; [cite: 215]

        case FWPS_CALLOUT_NOTIFY_DELETE_FILTER: [cite: 215]
             KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "BandwidthThrottler: Filter deleted (Callout ID: %u, Filter ID: %llu)\n", filter->action.calloutId, filter->filterId)); [cite: 215]
            break; [cite: 215]

        // case FWPS_CALLOUT_NOTIFY_TYPE_MAX: // Handle enum max value if used inadvertently [cite: 216]
        //     break; [cite: 216]
        default:
             // Handle other notification types if necessary
             break;
    }

    return STATUS_SUCCESS; [cite: 216]
}


// Flow Delete Function - Crucial for cleanup [cite: 217]
_Use_decl_annotations_
void NTAPI CalloutFlowDelete(
    UINT16 layerId,
    UINT32 calloutId,
    UINT64 flowContext // This is our PFLOW_CONTEXT [cite: 217]
)
{
    PFLOW_CONTEXT pFlowContext = (PFLOW_CONTEXT)flowContext; [cite: 217]
    UNREFERENCED_PARAMETER(layerId); [cite: 218]
    UNREFERENCED_PARAMETER(calloutId); [cite: 218]

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_TRACE_LEVEL, "BandwidthThrottler: FlowDeleteFn called for context %p\n", pFlowContext)); [cite: 218]

    if (pFlowContext) { [cite: 219]
        // Dereference the rule this flow context was pointing to [cite: 219]
        if(pFlowContext->Rule) { // Check rule pointer is valid before dereferencing
             DereferenceRule(pFlowContext->Rule); [cite: 219]
        }
        // Free the flow context itself [cite: 219]
        ExFreePoolWithTag(pFlowContext, POOL_TAG); [cite: 220]
    }
}

// Add `#pragma comment(lib, "rpcrt4.lib")` if UuidCreate is used and linker complains
