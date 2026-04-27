////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////

#ifndef __DWUDF_REGISTRY__H__
#define __DWUDF_REGISTRY__H__

#define         UDF_BM_FLUSH_PERIOD_NAME    L"BitmapFlushPeriod"
#define         UDF_TREE_FLUSH_PERIOD_NAME  L"DirTreeFlushPeriod"
#define         UDF_NO_UPDATE_PERIOD_NAME   L"MaxNoUpdatePeriod"
#define         UDF_SPARSE_THRESHOLD_NAME   L"SparseThreshold"
#define         UDF_UPDATE_TIMES_ATTR       L"UpdateFileTimesAttrChg"
#define         UDF_UPDATE_TIMES_MOD        L"UpdateFileTimesLastWrite"
#define         UDF_UPDATE_TIMES_ACCS       L"UpdateFileTimesLastAccess"
#define         UDF_UPDATE_ATTR_ARCH        L"UpdateFileAttrArchive"
#define         UDF_UPDATE_DIR_TIMES_ATTR_W L"UpdateDirAttrAndTimesOnModify"
#define         UDF_UPDATE_DIR_TIMES_ATTR_R L"UpdateDirAttrAndTimesOnAccess"
#define         UDF_ALLOW_UPDATE_TIMES_ACCS_UCHG_DIR L"AllowUpdateAccessTimeInUnchangedDir"
#define         UDF_INSTANT_COMPAT_ALLOC_DESCS  L"AllocDescCompatInstantBurner"
#define         UDF_HANDLE_HW_RO            L"HandleHWReadOnly"
#define         UDF_HANDLE_SOFT_RO          L"HandleSoftReadOnly"
#define         UDF_IGNORE_SEQUENTIAL_IO    L"IgnoreSequantialIo"
#define         UDF_NO_SPARE_BEHAVIOR       L"NoFreeRelocationSpaceVolumeAction"
#define         UDF_DIRTY_VOLUME_BEHAVIOR   L"DirtyVolumeVolumeAction"

#endif //__DWUDF_REGISTRY__H__
