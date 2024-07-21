////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: UDFinit.cpp
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
UDFData                 UDFGlobalData;

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
            RtlZeroMemory(&UDFGlobalData, sizeof(UDFGlobalData));

            // initialize some required fields
            UDFGlobalData.NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_GLOBAL_DATA;
            UDFGlobalData.NodeIdentifier.NodeByteSize = sizeof(UDFGlobalData);

            // initialize the global data resource and remember the fact that
            //  the resource has been initialized
            RC = UDFInitializeResourceLite(&(UDFGlobalData.GlobalDataResource));
            ASSERT(NT_SUCCESS(RC));
            SetFlag(UDFGlobalData.UDFFlags, UDF_DATA_FLAGS_RESOURCE_INITIALIZED);

            RC = UDFInitializeResourceLite(&(UDFGlobalData.DelayedCloseResource));
            ASSERT(NT_SUCCESS(RC));
//            SetFlag(UDFGlobalData.UDFFlags, UDF_DATA_FLAGS_RESOURCE_INITIALIZED);

            // keep a ptr to the driver object sent to us by the I/O Mgr
            UDFGlobalData.DriverObject = DriverObject;

            //SeEnableAccessToExports();

            // initialize the mounted logical volume list head
            InitializeListHead(&(UDFGlobalData.VCBQueue));

            UDFPrint(("UDF: Init memory manager\n"));
            // Initialize internal memory management
            if(!MyAllocInit()) {
                try_return(RC = STATUS_INSUFFICIENT_RESOURCES);
            }
            InternalMMInitialized = TRUE;

            // before we proceed with any more initialization, read in
            //  user supplied configurable values ...

            // Save RegistryPath
            RtlCopyMemory(&(UDFGlobalData.SavedRegPath), RegistryPath, sizeof(UNICODE_STRING));

            UDFGlobalData.SavedRegPath.Buffer = (PWSTR)MyAllocatePool__(NonPagedPool, RegistryPath->Length + 2);
            if(!UDFGlobalData.SavedRegPath.Buffer) try_return (RC = STATUS_INSUFFICIENT_RESOURCES);
            RtlCopyMemory(UDFGlobalData.SavedRegPath.Buffer, RegistryPath->Buffer, RegistryPath->Length + 2);

            RegTGetKeyHandle(NULL, UDFGlobalData.SavedRegPath.Buffer, &hUdfRootKey);

            RtlInitUnicodeString(&UDFGlobalData.UnicodeStrRoot, L"\\");
            RtlInitUnicodeString(&UDFGlobalData.UnicodeStrSDir, L":");
            RtlInitUnicodeString(&UDFGlobalData.AclName, UDF_SN_NT_ACL);

            UDFPrint(("UDF: Init delayed close queues\n"));
#ifdef UDF_DELAYED_CLOSE
            InitializeListHead( &UDFGlobalData.DelayedCloseQueue );
            InitializeListHead( &UDFGlobalData.DirDelayedCloseQueue );

            ExInitializeWorkItem( &UDFGlobalData.CloseItem,
                                  UDFDelayedClose,
                                  NULL );

            UDFGlobalData.DelayedCloseCount = 0;
            UDFGlobalData.DirDelayedCloseCount = 0;
#endif //UDF_DELAYED_CLOSE

            // we should have the registry data (if any), allocate zone memory ...
            //  This is an example of when FSD implementations __try to pre-allocate
            //  some fixed amount of memory to avoid internal fragmentation and/or waiting
            //  later during run-time ...

            UDFPrint(("UDF: Init zones\n"));
            if (!NT_SUCCESS(RC = UDFInitializeZones()))
                try_return(RC);

            UDFPrint(("UDF: Init pointers\n"));
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

            UDFGlobalData.CPU_Count = KeNumberProcessors;

            UDFPrint(("UDF: Create CD dev obj\n"));
            if (!NT_SUCCESS(RC = UDFCreateFsDeviceObject(UDF_FS_NAME_CD,
                                    DriverObject,
                                    FILE_DEVICE_CD_ROM_FILE_SYSTEM,
                                    &(UDFGlobalData.UDFDeviceObject_CD)))) {
                // failed to create a device object, leave ...
                try_return(RC);
            }

            UDFPrint(("UDF: Create HDD dev obj\n"));
            if (!NT_SUCCESS(RC = UDFCreateFsDeviceObject(UDF_FS_NAME_HDD,
                                    DriverObject,
                                    FILE_DEVICE_DISK_FILE_SYSTEM,
                                    &(UDFGlobalData.UDFDeviceObject_HDD)))) {
                // failed to create a device object, leave ...
                try_return(RC);
            }

            if (UDFGlobalData.UDFDeviceObject_CD) {
                UDFPrint(("UDFCreateFsDeviceObject: IoRegisterFileSystem() for CD\n"));
                IoRegisterFileSystem(UDFGlobalData.UDFDeviceObject_CD);
            }

            if (UDFGlobalData.UDFDeviceObject_HDD) {
                UDFPrint(("UDFCreateFsDeviceObject: IoRegisterFileSystem() for HDD\n"));
                IoRegisterFileSystem(UDFGlobalData.UDFDeviceObject_HDD);
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
            if (UDFGlobalData.UDFDeviceObject_CD) {
                IoDeleteDevice(UDFGlobalData.UDFDeviceObject_CD);
                UDFGlobalData.UDFDeviceObject_CD = NULL;
            }

            if (UDFGlobalData.UDFDeviceObject_HDD) {
                IoDeleteDevice(UDFGlobalData.UDFDeviceObject_HDD);
                UDFGlobalData.UDFDeviceObject_HDD = NULL;
            }

            // free up any memory we might have reserved for zones/lookaside
            //  lists
            if (UDFGlobalData.UDFFlags & UDF_DATA_FLAGS_ZONES_INITIALIZED) {
                UDFDestroyZones();
            }

            // delete the resource we may have initialized
            if (UDFGlobalData.UDFFlags & UDF_DATA_FLAGS_RESOURCE_INITIALIZED) {
                // un-initialize this resource
                UDFDeleteResource(&(UDFGlobalData.GlobalDataResource));
                ClearFlag(UDFGlobalData.UDFFlags, UDF_DATA_FLAGS_RESOURCE_INITIALIZED);
            }
//        } else {
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

    // initialize the function pointers for the IRP major
    //  functions that this FSD is prepared to  handle ...
    //  NT Version 4.0 has 28 possible functions that a
    //  kernel mode driver can handle.
    //  NT Version 3.51 and before has only 22 such functions,
    //  of which 18 are typically interesting to most FSD's.

    //  The only interesting new functions that a FSD might
    //  want to respond to beginning with Version 4.0 are the
    //  IRP_MJ_QUERY_QUOTA and the IRP_MJ_SET_QUOTA requests.

    //  The code below does not handle quota manipulation, neither
    //  does the NT Version 4.0 operating system (or I/O Manager).
    //  However, you should be on the lookout for any such new
    //  functionality that the FSD might have to implement in
    //  the near future.

    DriverObject->MajorFunction[IRP_MJ_CREATE]              = UDFCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]               = UDFClose;
    DriverObject->MajorFunction[IRP_MJ_READ]                = UDFRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE]               = UDFWrite;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION]   = UDFQueryInfo;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION]     = UDFSetInfo;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]       = UDFFlush;

    // To implement support for querying and modifying volume attributes
    // (volume information query/set operations), enable initialization
    // of the following two function pointers and then implement the supporting
    // functions.
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = UDFQueryVolInfo;
    DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION] = UDFSetVolInfo;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL]   = UDFDirControl;
    // To implement support for file system IOCTL calls, enable initialization
    // of the following function pointer and implement appropriate support.
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = UDFFSControl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]      = UDFDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]            = UDFShutdown;
    // For byte-range lock support, enable initialization of the following
    // function pointer and implement appropriate support.
    DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL]        = UDFLockControl;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]             = UDFCleanup;

//    if(MajorVersion >= 0x05) {
        // w2k and higher
//        DriverObject->MajorFunction[IRP_MJ_PNP]           = UDFPnp;
//    }

    // Now, it is time to initialize the fast-io stuff ...
    PtrFastIoDispatch = DriverObject->FastIoDispatch = &UDFGlobalData.UDFFastIoDispatch;

    // initialize the global fast-io structure
    //  NOTE: The fast-io structure has undergone a substantial revision
    //  in Windows NT Version 4.0. The structure has been extensively expanded.
    //  Therefore, if the driver needs to work on both V3.51 and V4.0+,
    //  we will have to be able to distinguish between the two versions at compile time.

    RtlZeroMemory(PtrFastIoDispatch, sizeof(FAST_IO_DISPATCH));

    PtrFastIoDispatch->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    PtrFastIoDispatch->FastIoCheckIfPossible    = UDFFastIoCheckIfPossible;
    PtrFastIoDispatch->FastIoRead               = FsRtlCopyRead;
    PtrFastIoDispatch->FastIoWrite              = UDFFastIoCopyWrite /*FsRtlCopyWrite*/;
    PtrFastIoDispatch->FastIoQueryBasicInfo     = UDFFastIoQueryBasicInfo;
    PtrFastIoDispatch->FastIoQueryStandardInfo  = UDFFastIoQueryStdInfo;
    PtrFastIoDispatch->FastIoLock               = UDFFastLock;         // Lock
    PtrFastIoDispatch->FastIoUnlockSingle       = UDFFastUnlockSingle; // UnlockSingle
    PtrFastIoDispatch->FastIoUnlockAll          = UDFFastUnlockAll;    // UnlockAll
    PtrFastIoDispatch->FastIoUnlockAllByKey     =  (unsigned char (__stdcall *)(struct _FILE_OBJECT *,
        PVOID ,unsigned long,struct _IO_STATUS_BLOCK *,struct _DEVICE_OBJECT *))UDFFastUnlockAllByKey;     //  UnlockAllByKey

    //  This callback has been replaced by UDFFilterCallbackAcquireForCreateSection

    PtrFastIoDispatch->AcquireFileForNtCreateSection = NULL;
    PtrFastIoDispatch->ReleaseFileForNtCreateSection = UDFFastIoRelCreateSec;

//    PtrFastIoDispatch->FastIoDeviceControl = UDFFastIoDeviceControl;

    // the remaining are only valid under NT Version 4.0 and later
#if(_WIN32_WINNT >= 0x0400)

    PtrFastIoDispatch->FastIoQueryNetworkOpenInfo = UDFFastIoQueryNetInfo;

    PtrFastIoDispatch->AcquireForModWrite       = UDFFastIoAcqModWrite;
    PtrFastIoDispatch->ReleaseForModWrite       = UDFFastIoRelModWrite;
    PtrFastIoDispatch->AcquireForCcFlush        = UDFFastIoAcqCcFlush;
    PtrFastIoDispatch->ReleaseForCcFlush        = UDFFastIoRelCcFlush;

/*    // MDL functionality

    PtrFastIoDispatch->MdlRead                  = UDFFastIoMdlRead;
    PtrFastIoDispatch->MdlReadComplete          = UDFFastIoMdlReadComplete;
    PtrFastIoDispatch->PrepareMdlWrite          = UDFFastIoPrepareMdlWrite;
    PtrFastIoDispatch->MdlWriteComplete         = UDFFastIoMdlWriteComplete;*/

    //  this FSD does not support compressed read/write functionality,
    //  NTFS does, and if we design a FSD that can provide such functionality,
    //  we should consider initializing the fast io entry points for reading
    //  and/or writing compressed data ...
#endif  // (_WIN32_WINNT >= 0x0400)

    // last but not least, initialize the Cache Manager callback functions
    //  which are used in CcInitializeCacheMap()

    UDFGlobalData.CacheMgrCallBacks.AcquireForLazyWrite  = UDFAcqLazyWrite;
    UDFGlobalData.CacheMgrCallBacks.ReleaseFromLazyWrite = UDFRelLazyWrite;
    UDFGlobalData.CacheMgrCallBacks.AcquireForReadAhead  = UDFAcqReadAhead;
    UDFGlobalData.CacheMgrCallBacks.ReleaseFromReadAhead = UDFRelReadAhead;

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
    PUDFFS_DEV_EXTENSION FSDevExt;
    RtlInitUnicodeString(&DriverDeviceName, FsDeviceName);
    *DeviceObject = NULL;

    UDFPrint(("UDFCreateFsDeviceObject: create dev\n"));

    if (!NT_SUCCESS(RC = IoCreateDevice(
            DriverObject,                   // our driver object
            sizeof(UDFFS_DEV_EXTENSION),    // don't need an extension for this object
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
    FSDevExt = (PUDFFS_DEV_EXTENSION)((*DeviceObject)->DeviceExtension);
    // Zero it out (typically this has already been done by the I/O
    // Manager but it does not hurt to do it again)!
    RtlZeroMemory(FSDevExt, sizeof(UDFFS_DEV_EXTENSION));

    // Initialize the signature fields
    FSDevExt->NodeIdentifier.NodeTypeCode = UDF_NODE_TYPE_UDFFS_DEVOBJ;
    FSDevExt->NodeIdentifier.NodeByteSize = sizeof(UDFFS_DEV_EXTENSION);
    // register the driver with the I/O Manager, pretend as if this is
    //  a physical disk based FSD (or in order words, this FSD manages
    //  logical volumes residing on physical disk drives)
/*    UDFPrint(("UDFCreateFsDeviceObject: IoRegisterFileSystem()\n"));
    IoRegisterFileSystem(*DeviceObject);*/
    return(RC);
} // end UDFCreateFsDeviceObject()
