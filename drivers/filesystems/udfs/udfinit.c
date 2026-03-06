////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: UDFinit.c
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*     This file contains the initialization code for the kernel mode
*     UDF FSD module. The DriverEntry() routine is called by the I/O
*     sub-system to initialize the FSD.
*
*************************************************************************/

#include            "udffs.h"

// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID                UDF_FILE_INIT

// global variables are declared here
UDFData                 UdfData;

NTSTATUS
UDFCreateFsDeviceObject(
    PCWSTR          FsDeviceName,
    PDRIVER_OBJECT  DriverObject,
    DEVICE_TYPE     DeviceType,
    PDEVICE_OBJECT  *DeviceObject);

/*************************************************************************
*
* Function: DriverEntry()
*
* Description:
*   This routine is the standard entry point for all kernel mode drivers.
*   The routine is invoked at IRQL PASSIVE_LEVEL in the context of a
*   system worker thread.
*   All FSD specific data structures etc. are initialized here.
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: STATUS_SUCCESS/Error (will cause driver to be unloaded).
*
*************************************************************************/
NTSTATUS
NTAPI
DriverEntry(
    PDRIVER_OBJECT  DriverObject,       // created by the I/O sub-system
    PUNICODE_STRING RegistryPath        // path to the registry key
    )
{
    NTSTATUS        RC = STATUS_SUCCESS;
    BOOLEAN         InternalMMInitialized = FALSE;
    HKEY            hUdfRootKey;

    _SEH2_TRY {
        _SEH2_TRY {

#ifdef __REACTOS__
            UDFPrint(("UDF Init: OS should be ReactOS\n"));
#endif

            // initialize the global data structure
            RtlZeroMemory(&UdfData, sizeof(UdfData));

            // initialize some required fields
            UdfData.NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_GLOBAL_DATA;
            UdfData.NodeIdentifier.NodeByteSize = sizeof(UdfData);

            ExInitializeResourceLite(&UdfData.GlobalDataResource);

            // keep a ptr to the driver object sent to us by the I/O Mgr
            UdfData.DriverObject = DriverObject;

            //SeEnableAccessToExports();

            // initialize the mounted logical volume list head
            InitializeListHead(&(UdfData.VcbQueue));

            UDFPrint(("UDF: Init memory manager\n"));
            // Initialize internal memory management
            if (!MyAllocInit()) {
                try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
            }
            InternalMMInitialized = TRUE;

            // before we proceed with any more initialization, read in
            //  user supplied configurable values ...

            // Save RegistryPath
            RtlCopyMemory(&(UdfData.SavedRegPath), RegistryPath, sizeof(UNICODE_STRING));

            UdfData.SavedRegPath.Buffer = (PWSTR)MyAllocatePool__(NonPagedPool, RegistryPath->Length + 2);
            if (!UdfData.SavedRegPath.Buffer) try_return (RC = STATUS_INSUFFICIENT_RESOURCES);
            RtlCopyMemory(UdfData.SavedRegPath.Buffer, RegistryPath->Buffer, RegistryPath->Length + 2);

            RegTGetKeyHandle(NULL, UdfData.SavedRegPath.Buffer, &hUdfRootKey);

            RtlInitUnicodeString(&UdfData.UnicodeStrRoot, L"\\");
            RtlInitUnicodeString(&UdfData.UnicodeStrSDir, L":");
            RtlInitUnicodeString(&UdfData.AclName, UDF_SN_NT_ACL);

            ExInitializeFastMutex(&UdfData.UdfDataMutex);
            InitializeListHead(&UdfData.DelayedCloseQueue);
            InitializeListHead(&UdfData.AsyncCloseQueue);

            ExInitializeWorkItem(&UdfData.CloseItem,
                                 (PWORKER_THREAD_ROUTINE)UDFFspClose,
                                 NULL);

            UdfData.DelayedCloseCount = 0;

            // determine memory requirements

            switch (MmQuerySystemSize()) {
            case MmMediumSystem:
                UdfData.MaxDelayedCloseCount = 32;
                UdfData.MinDelayedCloseCount = 8;
                break;
            case MmLargeSystem:
                UdfData.MaxDelayedCloseCount = 72;
                UdfData.MinDelayedCloseCount = 18;
                break;
            case MmSmallSystem:
            default:
                UdfData.MaxDelayedCloseCount = 10;
                UdfData.MinDelayedCloseCount = 2;
            }

            ExInitializeNPagedLookasideList(&UdfData.IrpContextLookasideList,
                                            NULL,
                                            NULL,
                                            POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                            sizeof(IRP_CONTEXT),
                                            TAG_IRP_CONTEXT,
                                            0);

            // TODO: move to Paged?
            ExInitializeNPagedLookasideList(&UdfData.ObjectNameLookasideList,
                                            NULL,
                                            NULL,
                                            POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                            sizeof(UDFObjectName),
                                            TAG_OBJECT_NAME,
                                            0);

            ExInitializeNPagedLookasideList(&UdfData.NonPagedFcbLookasideList,
                                            NULL,
                                            NULL,
                                            POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                            sizeof(FCB),
                                            TAG_FCB_NONPAGED,
                                            0);

            ExInitializeNPagedLookasideList(&UdfData.UDFNonPagedFcbLookasideList,
                                            NULL,
                                            NULL,
                                            POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                            sizeof(FCB_NONPAGED),
                                            TAG_FCB_NONPAGED,
                                            0);

            ExInitializePagedLookasideList(&UdfData.UDFFcbIndexLookasideList,
                                           NULL,
                                           NULL,
                                           POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                           sizeof(FCB), //TODO:
                                           TAG_FCB_NONPAGED,
                                           0);

            ExInitializePagedLookasideList(&UdfData.UDFFcbDataLookasideList,
                                           NULL,
                                           NULL,
                                           POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                           sizeof(FCB), //TODO:
                                           TAG_FCB_NONPAGED,
                                           0);

            ExInitializePagedLookasideList(&UdfData.CcbLookasideList,
                                            NULL,
                                            NULL,
                                            POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                            sizeof(CCB),
                                            TAG_CCB,
                                            0);

            ExInitializePagedLookasideList(&UdfData.LcbLookasideList,
                                            NULL,
                                            NULL,
                                            POOL_NX_ALLOCATION | POOL_RAISE_IF_ALLOCATION_FAILURE,
                                            SIZEOF_LOOKASIDE_LCB,
                                            TAG_LCB,
                                            0);

            // initialize the IRP major function table, and the fast I/O table
            UDFInitializeFunctionPointers(DriverObject);

            //  Initialize the filter callbacks we use

            FS_FILTER_CALLBACKS FilterCallbacks;
            RtlZeroMemory(&FilterCallbacks, sizeof(FS_FILTER_CALLBACKS));

            FilterCallbacks.SizeOfFsFilterCallbacks = sizeof(FS_FILTER_CALLBACKS);
            FilterCallbacks.PreAcquireForSectionSynchronization = UDFFilterCallbackAcquireForCreateSection;

            RC = FsRtlRegisterFileSystemFilterCallbacks(DriverObject, &FilterCallbacks);
            if (!NT_SUCCESS(RC))
                try_return(RC);

            UDFPrint(("UDF: Create CD dev obj\n"));
            if (!NT_SUCCESS(RC = UDFCreateFsDeviceObject(UDF_FS_NAME_CD,
                                    DriverObject,
                                    FILE_DEVICE_CD_ROM_FILE_SYSTEM,
                                    &(UdfData.UDFDeviceObject_CD)))) {
                // failed to create a device object, leave ...
                try_return(RC);
            }

            UDFPrint(("UDF: Create HDD dev obj\n"));
            if (!NT_SUCCESS(RC = UDFCreateFsDeviceObject(UDF_FS_NAME_HDD,
                                    DriverObject,
                                    FILE_DEVICE_DISK_FILE_SYSTEM,
                                    &(UdfData.UDFDeviceObject_HDD)))) {
                // failed to create a device object, leave ...
                try_return(RC);
            }

            if (UdfData.UDFDeviceObject_CD) {
                UDFPrint(("UDFCreateFsDeviceObject: IoRegisterFileSystem() for CD\n"));
                IoRegisterFileSystem(UdfData.UDFDeviceObject_CD);
            }

            if (UdfData.UDFDeviceObject_HDD) {
                UDFPrint(("UDFCreateFsDeviceObject: IoRegisterFileSystem() for HDD\n"));
                IoRegisterFileSystem(UdfData.UDFDeviceObject_HDD);
            }

            RC = STATUS_SUCCESS;

        } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            // we encountered an exception somewhere, eat it up
            UDFPrint(("UDF: exception\n"));
            RC = _SEH2_GetExceptionCode();
        } _SEH2_END;

        InternalMMInitialized = FALSE;

        try_exit:   NOTHING;
    } _SEH2_FINALLY {
        // start unwinding if we were unsuccessful
        if (!NT_SUCCESS(RC)) {
            UDFPrint(("UDF: failed with status %x\n", RC));
            // Now, delete any device objects, etc. we may have created

            if (InternalMMInitialized) {
                MyAllocRelease();
            }
            if (UdfData.UDFDeviceObject_CD) {
                IoDeleteDevice(UdfData.UDFDeviceObject_CD);
                UdfData.UDFDeviceObject_CD = NULL;
            }

            if (UdfData.UDFDeviceObject_HDD) {
                IoDeleteDevice(UdfData.UDFDeviceObject_HDD);
                UdfData.UDFDeviceObject_HDD = NULL;
            }
        }
    } _SEH2_END;

    return(RC);
} // end DriverEntry()



/*************************************************************************
*
* Function: UDFInitializeFunctionPointers()
*
* Description:
*   Initialize the IRP... function pointer array in the driver object
*   structure. Also initialize the fast-io function ptr array ...
*
* Expected Interrupt Level (for execution) :
*
*  IRQL_PASSIVE_LEVEL
*
* Return Value: None
*
*************************************************************************/
VOID
NTAPI
UDFInitializeFunctionPointers(
    PDRIVER_OBJECT      DriverObject       // created by the I/O sub-system
    )
{
    PFAST_IO_DISPATCH    PtrFastIoDispatch = NULL;

#pragma prefast(push)
#pragma prefast(disable: 28155, "the dispatch routine has the correct type, prefast is just being paranoid.")
#pragma prefast(disable: 28168, "the dispatch routine has the correct type, prefast is just being paranoid.")
#pragma prefast(disable: 28169, "the dispatch routine has the correct type, prefast is just being paranoid.")
#pragma prefast(disable: 28175, "we're allowed to change these.")

    // Note that because of the way data caching is done, we set neither
    // the Direct I/O or Buffered I/O bit in DeviceObject->Flags.  If
    // data is not in the cache, or the request is not buffered, we may,
    // set up for Direct I/O by hand.

    // Initialize the driver object with this driver's entry points.
    //
    // NOTE - Each entry in the dispatch table must have an entry in
    // the Fsp/Fsd dispatch switch statements.

    DriverObject->MajorFunction[IRP_MJ_CREATE]              =
    DriverObject->MajorFunction[IRP_MJ_CLOSE]               =
    DriverObject->MajorFunction[IRP_MJ_READ]                =
    DriverObject->MajorFunction[IRP_MJ_WRITE]               =
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION]   =
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION]     =
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]       =

    // To implement support for querying and modifying volume attributes
    // (volume information query/set operations), enable initialization
    // of the following two function pointers and then implement the supporting
    // functions.
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] =
    DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION] =
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL]   =
    // To implement support for file system IOCTL calls, enable initialization
    // of the following function pointer and implement appropriate support.
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] =
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]      =
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]            =
    // For byte-range lock support, enable initialization of the following
    // function pointer and implement appropriate support.
    DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL]        =
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]             =

    DriverObject->MajorFunction[IRP_MJ_PNP]                 = (PDRIVER_DISPATCH)UDFFsdDispatch;
#pragma prefast(pop)

    // Now, it is time to initialize the fast-io stuff ...
    PtrFastIoDispatch = DriverObject->FastIoDispatch = &UdfData.UDFFastIoDispatch;

    // initialize the global fast-io structure
    //  NOTE: The fast-io structure has undergone a substantial revision
    //  in Windows NT Version 4.0. The structure has been extensively expanded.
    //  Therefore, if the driver needs to work on both V3.51 and V4.0+,
    //  we will have to be able to distinguish between the two versions at compile time.

    RtlZeroMemory(PtrFastIoDispatch, sizeof(FAST_IO_DISPATCH));

    PtrFastIoDispatch->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    PtrFastIoDispatch->FastIoCheckIfPossible    = UDFFastIoCheckIfPossible;
    PtrFastIoDispatch->FastIoRead               = UDFFastIoCopyRead;
    PtrFastIoDispatch->FastIoWrite              = UDFFastIoCopyWrite;
    PtrFastIoDispatch->FastIoQueryBasicInfo     = UDFFastIoQueryBasicInfo;
    PtrFastIoDispatch->FastIoQueryStandardInfo  = UDFFastIoQueryStdInfo;
    PtrFastIoDispatch->FastIoLock               = UDFFastLock;         // Lock
    PtrFastIoDispatch->FastIoUnlockSingle       = UDFFastUnlockSingle; // UnlockSingle
    PtrFastIoDispatch->FastIoUnlockAll          = UDFFastUnlockAll;    // UnlockAll
    PtrFastIoDispatch->FastIoUnlockAllByKey     = UDFFastUnlockAllByKey; //  UnlockAllByKey

    //  This callback has been replaced by UDFFilterCallbackAcquireForCreateSection

    PtrFastIoDispatch->AcquireFileForNtCreateSection = NULL;
    PtrFastIoDispatch->ReleaseFileForNtCreateSection = UDFFastIoRelCreateSec;

    PtrFastIoDispatch->FastIoQueryNetworkOpenInfo = UDFFastIoQueryNetInfo;

    PtrFastIoDispatch->AcquireForModWrite       = UDFFastIoAcqModWrite;
    PtrFastIoDispatch->ReleaseForModWrite       = NULL;
    PtrFastIoDispatch->AcquireForCcFlush        = UDFFastIoAcqCcFlush;
    PtrFastIoDispatch->ReleaseForCcFlush        = UDFFastIoRelCcFlush;

/*    // MDL functionality

    PtrFastIoDispatch->MdlRead                  = UDFFastIoMdlRead;
    PtrFastIoDispatch->MdlReadComplete          = UDFFastIoMdlReadComplete;
    PtrFastIoDispatch->PrepareMdlWrite          = UDFFastIoPrepareMdlWrite;
    PtrFastIoDispatch->MdlWriteComplete         = UDFFastIoMdlWriteComplete;*/

    // last but not least, initialize the Cache Manager callback functions
    //  which are used in CcInitializeCacheMap()

    UdfData.CacheMgrCallBacks.AcquireForLazyWrite  = UDFAcqLazyWrite;
    UdfData.CacheMgrCallBacks.ReleaseFromLazyWrite = UDFRelLazyWrite;
    UdfData.CacheMgrCallBacks.AcquireForReadAhead  = UDFAcqReadAhead;
    UdfData.CacheMgrCallBacks.ReleaseFromReadAhead = UDFRelReadAhead;

    DriverObject->DriverUnload = UDFDriverUnload;

    return;
} // end UDFInitializeFunctionPointers()

NTSTATUS
UDFCreateFsDeviceObject(
    PCWSTR          FsDeviceName,
    PDRIVER_OBJECT  DriverObject,
    DEVICE_TYPE     DeviceType,
    PDEVICE_OBJECT  *DeviceObject
    )
{
    NTSTATUS RC = STATUS_SUCCESS;
    UNICODE_STRING  DriverDeviceName;
    RtlInitUnicodeString(&DriverDeviceName, FsDeviceName);
    *DeviceObject = NULL;

    UDFPrint(("UDFCreateFsDeviceObject: create dev\n"));

    if (!NT_SUCCESS(RC = IoCreateDevice(
            DriverObject,                   // our driver object
            0,
            &DriverDeviceName,              // name - can be used to "open" the driver
                                // see the book for alternate choices
            DeviceType,
            0,                  // no special characteristics
                                // do not want this as an exclusive device, though you might
            FALSE,
            DeviceObject))) {
                // failed to create a device object, leave ...
        return(RC);
    }

    return(RC);
} // end UDFCreateFsDeviceObject()
