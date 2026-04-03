////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
#include "udffs.h"

VOID
NTAPI
UDFDriverUnload(
    IN PDRIVER_OBJECT DriverObject
    )
{
    UDFPrint(("UDF: Unloading!!\n"));

    // Release the object references we took in DriverEntry after
    // IoRegisterFileSystem, matching Fastfat's FatUnload pattern.
    if (UdfData.UDFDeviceObject_CD) {
        ObDereferenceObject(UdfData.UDFDeviceObject_CD);
    }
    if (UdfData.UDFDeviceObject_HDD) {
        ObDereferenceObject(UdfData.UDFDeviceObject_HDD);
    }
}
