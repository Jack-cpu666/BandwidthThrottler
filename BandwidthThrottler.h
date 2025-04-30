#ifndef BANDWIDTH_THROTTLER_H
#define BANDWIDTH_THROTTLER_H

#include <ntddk.h>
#include <fwpmk.h> // WFP Kernel-Mode API
#include <fwpsk.h> // WFP Kernel-Mode Callout API
#include <guiddef.h>
#include <initguid.h> // Required for DEFINE_GUID macros
#include <ntstrsafe.h>

// Define a pool tag for memory allocations
#define POOL_TAG 'rThB' // "BThr"

// --- REPLACE THESE WITH YOUR UNIQUE GUIDs ---
// Generated GUIDs (replace comments with actual generation method if needed)
// uuidgen.exe output formatted for DEFINE_GUID
DEFINE_GUID(BANDWIDTH_THROTTLER_SUBLAYER_GUID,
    0xe2b1c3d4, 0xf5a6, 0x7b8c, 0x9d, 0x0e, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e); // New GUID 1
DEFINE_GUID(BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V4,
    0x1a2b3c4d, 0x5e6f, 0x7a8b, 0x9c, 0x0d, 0xe1, 0xf2, 0xa3, 0xb4, 0xc5, 0xd6); // New GUID 2
DEFINE_GUID(BANDWIDTH_THROTTLER_CALLOUT_ALE_CONNECT_V6,
    0xf0e1d2c3, 0xb4a5, 0x9687, 0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f); // New GUID 3
DEFINE_GUID(BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V4,
    0x98765432, 0xabcd, 0xef10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18); // New GUID 4
DEFINE_GUID(BANDWIDTH_THROTTLER_CALLOUT_TRANSPORT_V6,
    0xabcdef01, 0x2345, 0x6789, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89); // New GUID 5
// --- END OF GUIDs TO REPLACE ---

// Device Name and IOCTL codes
#define NT_DEVICE_NAME      L"\\Device\\BandwidthThrottler"
#define DOS_DEVICE_NAME     L"\\DosDevices\\BandwidthThrottler"

// IOCTL Codes (make sure these match Python)
#define IOCTL_ADD_RULE      CTL_CODE(FILE_DEVICE_NETWORK, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_REMOVE_RULE   CTL_CODE(FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// #define IOCTL_QUERY_RULES CTL_CODE(FILE_DEVICE_NETWORK, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS) // Optional

// User-mode Rule structure (must match Python)
#pragma pack(push, 1) // Ensure structure packing matches Python
typedef struct _USER_RULE_DATA {
    ULONG ProcessId;
    ULONG UploadLimitKBps;
    ULONG DownloadLimitKBps;
} USER_RULE_DATA, *PUSER_RULE_DATA;
#pragma pack(pop)

// Internal Driver Structures

// Token Bucket state
typedef struct _TOKEN_BUCKET {
    KSPIN_LOCK Lock;
    UINT64 CapacityBytes;       // Max tokens (bytes)
    UINT64 CurrentTokensBytes;  // Current available tokens (bytes)
    UINT64 RefillRateBytesPerTick; // Bytes added per timer tick
    LARGE_INTEGER LastRefillTimestamp; // KeQueryInterruptTime timestamp
} TOKEN_BUCKET, *PTOKEN_BUCKET;

// Rule structure stored internally
typedef struct _THROTTLE_RULE {
    LIST_ENTRY Link;           // Linked list entry
    ULONG ProcessId;
    BOOLEAN Active;            // Flag if rule is actively used by flows
    TOKEN_BUCKET UploadBucket;
    TOKEN_BUCKET DownloadBucket;
    LONG ReferenceCount;       // Reference count for flow context association
} THROTTLE_RULE, *PTHROTTLE_RULE;

// Flow Context structure associated with WFP flows
typedef struct _FLOW_CONTEXT {
    PTHROTTLE_RULE Rule;        // Pointer to the rule governing this flow
    BOOLEAN IsUpload;           // True if this context is for upload traffic
} FLOW_CONTEXT, *PFLOW_CONTEXT;

// --- Global Variables ---
extern HANDLE g_WfpEngineHandle;
extern PDEVICE_OBJECT g_DeviceObject;
extern LIST_ENTRY g_RuleList;
extern KSPIN_LOCK g_RuleListLock;
extern KTIMER g_RefillTimer;
extern KDPC g_RefillDpc;
extern BOOLEAN g_DriverUnloading;

// --- Function Prototypes ---

// Driver Entry/Unload
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;

// WFP Registration/Unregistration
NTSTATUS WfpRegisterCallouts(PDEVICE_OBJECT deviceObject);
NTSTATUS WfpUnregisterCallouts();

// WFP Callout Functions
_IRQL_requires_same_
_Function_class_(FWPS_CALLOUT_CLASSIFY_FN)
void NTAPI CalloutClassifyALEConnect(
    _In_ const FWPS_INCOMING_VALUES* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES* inMetaValues,
    _Inout_opt_ void* layerData,
    _In_opt_ const void* classifyContext,
    _In_ const FWPS_FILTER* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT* classifyOut
);

_IRQL_requires_same_
_Function_class_(FWPS_CALLOUT_CLASSIFY_FN)
void NTAPI CalloutClassifyTransport(
    _In_ const FWPS_INCOMING_VALUES* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES* inMetaValues,
    _Inout_opt_ void* layerData,
    _In_opt_ const void* classifyContext,
    _In_ const FWPS_FILTER* filter,         // Corrected parameter name
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT* classifyOut  // Corrected parameter name
);

_IRQL_requires_same_
_Function_class_(FWPS_CALLOUT_NOTIFY_FN)
NTSTATUS NTAPI CalloutNotify(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE notifyType,
    _In_ const GUID* filterKey,
    _Inout_ FWPS_FILTER* filter             // Corrected parameter name
);

_IRQL_requires_same_
_Function_class_(FWPS_CALLOUT_FLOW_DELETE_NOTIFY_FN)
void NTAPI CalloutFlowDelete(
    _In_ UINT16 layerId,
    _In_ UINT32 calloutId,
    _In_ UINT64 flowContext
);

// IOCTL Handler
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
DRIVER_DISPATCH DeviceControl;
NTSTATUS AddRule(PUSER_RULE_DATA ruleData);
NTSTATUS RemoveRule(ULONG processId);

// Rule Management
_Requires_lock_held_(g_RuleListLock)
PTHROTTLE_RULE FindRuleByPid(ULONG processId);
void InitializeTokenBucket(PTOKEN_BUCKET bucket, ULONG limitKBps, UINT64 timerIntervalMillis);
BOOLEAN ConsumeTokens(PTOKEN_BUCKET bucket, SIZE_T bytes);

// Timer DPC
_Function_class_(KDEFERRED_ROUTINE)
VOID NTAPI TimerDpcRoutine(
    _In_ struct _KDPC *Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
);

VOID StartRefillTimer();
VOID StopRefillTimer();

// Utility
VOID CleanupRules();
VOID DereferenceRule(PTHROTTLE_RULE rule);


#endif // BANDWIDTH_THROTTLER_H
