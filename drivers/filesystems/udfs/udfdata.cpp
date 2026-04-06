////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: Close.cpp
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   Contains code to handle the "Close" dispatch entry point.
*
*************************************************************************/

#include            "udffs.h"

_IRQL_requires_max_(APC_LEVEL)
__drv_dispatchType(DRIVER_DISPATCH)
__drv_dispatchType(IRP_MJ_CREATE)
__drv_dispatchType(IRP_MJ_CLOSE)
__drv_dispatchType(IRP_MJ_READ)
__drv_dispatchType(IRP_MJ_WRITE)
__drv_dispatchType(IRP_MJ_QUERY_INFORMATION)
__drv_dispatchType(IRP_MJ_SET_INFORMATION)
__drv_dispatchType(IRP_MJ_QUERY_VOLUME_INFORMATION)
__drv_dispatchType(IRP_MJ_DIRECTORY_CONTROL)
__drv_dispatchType(IRP_MJ_FILE_SYSTEM_CONTROL)
__drv_dispatchType(IRP_MJ_DEVICE_CONTROL)
__drv_dispatchType(IRP_MJ_LOCK_CONTROL)
__drv_dispatchType(IRP_MJ_CLEANUP)
__drv_dispatchType(IRP_MJ_PNP)
__drv_dispatchType(IRP_MJ_SHUTDOWN)
NTSTATUS
NTAPI
UDFFsdDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )

/*++

Routine Description:

    This is the driver entry to all of the Fsd dispatch points.

    Conceptually the Io routine will call this routine on all requests
    to the file system.  We case on the type of request and invoke the
    correct handler for this type of request.  There is an exception filter
    to catch any exceptions in the CDFS code as well as the CDFS process
    exception routine.

    This routine allocates and initializes the IrpContext for this request as
    well as updating the top-level thread context as necessary.  We may loop
    in this routine if we need to retry the request for any reason.  The
    status code STATUS_CANT_WAIT is used to indicate this.  Suppose the disk
    in the drive has changed.  An Fsd request will proceed normally until it
    recognizes this condition.  STATUS_VERIFY_REQUIRED is raised at that point
    and the exception code will handle the verify and either return
    STATUS_CANT_WAIT or STATUS_PENDING depending on whether the request was
    posted.

Arguments:

    DeviceObject - Supplies the volume device object for this request

    Irp - Supplies the Irp being processed

Return Value:

    NTSTATUS - The FSD status for the IRP

--*/

{
    THREAD_CONTEXT ThreadContext = {0};
    PIRP_CONTEXT IrpContext = NULL;
    BOOLEAN Wait = FALSE;

#ifdef UDF_SANITY
    PVOID PreviousTopLevel;
#endif

    NTSTATUS Status = STATUS_SUCCESS;

#if DBG

    KIRQL SaveIrql = KeGetCurrentIrql();

#endif

    ASSERT_OPTIONAL_IRP(Irp);

    UNREFERENCED_PARAMETER(DeviceObject);

    FsRtlEnterFileSystem();

#ifdef UDF_SANITY
    PreviousTopLevel = IoGetTopLevelIrp();
#endif

    // Loop until this request has been completed or posted.

    do {

        // Use a try-except to handle the exception cases.

        _SEH2_TRY {

            // If the IrpContext is NULL then this is the first pass through
            // this loop.

            if (IrpContext == NULL) {

                // Decide if this request is waitable an allocate the IrpContext.
                // If the file object in the stack location is NULL then this
                // is a mount which is always waitable.  Otherwise we look at
                // the file object flags.

                if (IoGetCurrentIrpStackLocation(Irp)->FileObject == NULL) {

                    Wait = TRUE;

                } else {

                    Wait = CanFsdWait(Irp);
                }

                IrpContext = UDFCreateIrpContext(Irp, Wait);

                // Update the thread context information.

                UDFSetThreadContext(IrpContext, &ThreadContext);

#ifdef UDF_SANITY
                NT_ASSERT(SafeNodeType(IrpContext->TopLevel) == UDF_NODE_TYPE_IRP_CONTEXT);
#endif

            // Otherwise cleanup the IrpContext for the retry.

            } else {

                // Set the MORE_PROCESSING flag to make sure the IrpContext
                // isn't inadvertently deleted here.  Then cleanup the
                // IrpContext to perform the retry.

                SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_MORE_PROCESSING);
                UDFCleanupIrpContext(IrpContext, FALSE);
            }

            // Case on the major irp code.

            switch (IrpContext->MajorFunction) {

            case IRP_MJ_CREATE:

                Status = UDFCommonCreate(IrpContext, Irp);
                break;

            case IRP_MJ_CLOSE:

                Status = UDFCommonClose(IrpContext, Irp);
                break;

            case IRP_MJ_READ:

                // If this is an Mdl complete request, don't go through
                // common read.

                if (FlagOn(IrpContext->MinorFunction, IRP_MN_COMPLETE)) {

                    Status = UDFCompleteMdl(IrpContext, Irp);

                } else {

                    Status = UDFCommonRead(IrpContext, Irp);
                }

                break;

            case IRP_MJ_WRITE:

                // If this is an Mdl complete request, don't go through
                // common write.

                if (FlagOn(IrpContext->MinorFunction, IRP_MN_COMPLETE)) {

                    Status = UDFCompleteMdl(IrpContext, Irp);

                } else {

                    Status = UDFCommonWrite(IrpContext, Irp);
                }

                break;

            case IRP_MJ_QUERY_INFORMATION:

                Status = UDFCommonQueryInfo(IrpContext, Irp);
                break;

            case IRP_MJ_SET_INFORMATION:

                Status = UDFCommonSetInfo(IrpContext, Irp);
                break;

            case IRP_MJ_FLUSH_BUFFERS:

                Status = UDFCommonFlush(IrpContext, Irp);
                break;

            case IRP_MJ_QUERY_VOLUME_INFORMATION:

                Status = UDFCommonQueryVolInfo(IrpContext, Irp);
                break;

            case IRP_MJ_SET_VOLUME_INFORMATION:

                Status = UDFCommonSetVolInfo(IrpContext, Irp);
                break;

            case IRP_MJ_DIRECTORY_CONTROL:

                Status = UDFCommonDirControl(IrpContext, Irp);
                break;

            case IRP_MJ_FILE_SYSTEM_CONTROL:

                Status = UDFCommonFsControl(IrpContext, Irp);
                break;

            case IRP_MJ_DEVICE_CONTROL:

                Status = UDFCommonDevControl(IrpContext, Irp);
                break;

            case IRP_MJ_LOCK_CONTROL:

                Status = UDFCommonLockControl(IrpContext, Irp);
                break;

            case IRP_MJ_CLEANUP:

                Status = UDFCommonCleanup(IrpContext, Irp);
                break;

            case IRP_MJ_PNP:

                Status = UDFCommonPnp(IrpContext, Irp);
                break;

            case IRP_MJ_SHUTDOWN:
            
                Status = UDFCommonShutdown(IrpContext, Irp);
                break;

            default :

                Status = STATUS_INVALID_DEVICE_REQUEST;
                UDFCompleteRequest(IrpContext, Irp, Status);
            }

        } _SEH2_EXCEPT(UDFExceptionFilter(IrpContext, _SEH2_GetExceptionInformation())) {

            Status = UDFProcessException(IrpContext, Irp, _SEH2_GetExceptionCode());
        } _SEH2_END;

    } while (Status == STATUS_CANT_WAIT);

#ifdef UDF_SANITY
    NT_ASSERT((PreviousTopLevel == IoGetTopLevelIrp()));
#endif

    FsRtlExitFileSystem();

    NT_ASSERT(SaveIrql == KeGetCurrentIrql());

    return Status;
}
