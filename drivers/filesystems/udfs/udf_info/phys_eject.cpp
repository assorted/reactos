////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*
 Module Name: Phys_eject.cpp

 Execution: Kernel mode only

 Description:

   Contains code that implement read/write operations for physical device
*/

#include            "udf.h"
// define the file specific bug-check id
#define         UDF_BUG_CHECK_ID        UDF_FILE_PHYS_EJECT

NTSTATUS
UDFDoDismountSequence(
    IN PVCB Vcb,
    IN BOOLEAN Eject
    )
{
    LARGE_INTEGER delay;
//    NTSTATUS      RC;
    ULONG i;

    // flush system cache
    UDFFlushVolume(NULL, Vcb);
    UDFPrint(("UDFDoDismountSequence:\n"));

    delay.QuadPart = -1000000; // 0.1 sec
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    UDFAcquireResourceExclusive(&(Vcb->IoResource), TRUE);

    // unlock media, drop our own Locks
    if (Vcb->VcbState & VCB_STATE_REMOVABLE_MEDIA) {
        UDFPrint(("  cleanup tray-lock (%d+2):\n", Vcb->MediaLockCount));
        for(i=0; i<Vcb->MediaLockCount+2; i++) {

            UDFToggleMediaEjectDisable(Vcb, FALSE);
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
        delay.QuadPart = -2000000; // 0.2 sec
    }

    if (!Vcb->ForgetVolume) {

        // eject media
        if (Eject &&
           (Vcb->VcbState & VCB_STATE_REMOVABLE_MEDIA)) {

            UDFPhSendIOCTL(IOCTL_STORAGE_EJECT_MEDIA,
                           Vcb->TargetDeviceObject,
                           NULL,0,
                           NULL,0,
                           FALSE,NULL);
        }
    }
    UDFReleaseResource(&(Vcb->IoResource));
    // allow media change checks (this will lead to dismount)
    // ... and make it Read-Only...  :-\~
    Vcb->VcbState &= ~UDF_VCB_FLAGS_MEDIA_LOCKED;

    return STATUS_SUCCESS;
} // end UDFDoDismountSequence()

