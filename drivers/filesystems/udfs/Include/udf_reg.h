////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////

#ifndef __DWUDF_REGISTRY__H__
#define __DWUDF_REGISTRY__H__

#define         REG_DEFAULT_UNKNOWN         L"_Default\\Unknown"
#define         REG_NAMELESS_DEV            L"\\_Nameless_"

#define         UDF_FS_TITLE_BLANK          L"Blank media"
#define         UDF_FS_TITLE_UNKNOWN        L"Unknown"
#define         UDF_BLANK_VOLUME_LABEL      L"Blank CD"
#define         REG_USEEXTENDEDFE_NAME      L"UseExtendedFE"
#define         REG_DEFALLOCMODE_NAME       L"DefaultAllocMode"
#define         UDF_DEFAULT_UID_NAME        L"DefaultUID"
#define         UDF_DEFAULT_GID_NAME        L"DefaultGID"
#define         UDF_DIR_PACK_THRESHOLD_NAME L"PackDirThreshold"
#define         UDF_FE_CHARGE_NAME          L"FECharge"
#define         UDF_FE_CHARGE_SDIR_NAME     L"FEChargeSDir"
#define         UDF_BM_FLUSH_PERIOD_NAME    L"BitmapFlushPeriod"
#define         UDF_TREE_FLUSH_PERIOD_NAME  L"DirTreeFlushPeriod"
#define         UDF_NO_UPDATE_PERIOD_NAME   L"MaxNoUpdatePeriod"
#define         UDF_NO_EJECT_PERIOD_NAME   L"MaxNoEjectPeriod"
#define         UDF_FSP_THREAD_PER_CPU_NAME L"ThreadsPerCpu"
#define         UDF_READAHEAD_GRAN_NAME     L"ReadAheadGranlarity"
#define         UDF_SPARSE_THRESHOLD_NAME   L"SparseThreshold"
#define         UDF_VERIFY_ON_WRITE_NAME    L"VerifyOnWrite"
#define         UDF_UPDATE_TIMES_ATTR       L"UpdateFileTimesAttrChg"
#define         UDF_UPDATE_TIMES_MOD        L"UpdateFileTimesLastWrite"
#define         UDF_UPDATE_TIMES_ACCS       L"UpdateFileTimesLastAccess"
#define         UDF_UPDATE_ATTR_ARCH        L"UpdateFileAttrArchive"
#define         UDF_UPDATE_DIR_TIMES_ATTR_W L"UpdateDirAttrAndTimesOnModify"
#define         UDF_UPDATE_DIR_TIMES_ATTR_R L"UpdateDirAttrAndTimesOnAccess"
#define         UDF_ALLOW_WRITE_IN_RO_DIR   L"AllowCreateInsideReadOnlyDirectory"
#define         UDF_ALLOW_UPDATE_TIMES_ACCS_UCHG_DIR L"AllowUpdateAccessTimeInUnchangedDir"
#define         UDF_W2K_COMPAT_ALLOC_DESCS  L"AllocDescCompatW2K"
#define         UDF_W2K_COMPAT_VLABEL       L"VolumeLabelCompatW2K"
#define         UDF_INSTANT_COMPAT_ALLOC_DESCS  L"AllocDescCompatInstantBurner"
#define         UDF_HANDLE_HW_RO            L"HandleHWReadOnly"
#define         UDF_HANDLE_SOFT_RO          L"HandleSoftReadOnly"
#define         UDF_FLUSH_MEDIA             L"FlushMedia"
#define         UDF_COMPARE_BEFORE_WRITE    L"CompareBeforeWrite"
#define         UDF_CACHE_SIZE_MULTIPLIER   L"WCacheSizeMultiplier"
#define         UDF_CHAINED_IO              L"CacheChainedIo"
#define         UDF_OS_NATIVE_DOS_NAME      L"UseOsNativeDOSName"
#define         UDF_FORCE_WRITE_THROUGH_NAME L"ForceWriteThrough"
#define         UDF_FORCE_HW_RO             L"ForceHWReadOnly"
#define         UDF_IGNORE_SEQUENTIAL_IO    L"IgnoreSequantialIo"
#define         UDF_PART_DAMAGED_BEHAVIOR   L"PartitialDamagedVolumeAction"
#define         UDF_NO_SPARE_BEHAVIOR       L"NoFreeRelocationSpaceVolumeAction"
#define         UDF_DIRTY_VOLUME_BEHAVIOR   L"DirtyVolumeVolumeAction"
#define         UDF_SHOW_BLANK_CD           L"ShowBlankCd"
#define         UDF_WAIT_CD_SPINUP          L"WaitCdSpinUpOnMount"
#define         UDF_AUTOFORMAT              L"Autoformat"
#define         UDF_CACHE_BAD_VDS           L"CacheBadVDSLocations"
#define         UDF_USE_EJECT_BUTTON        L"UseEjectButton"
#define         UDF_LICENSE_KEY             L"LicenseKey"


#endif //__DWUDF_REGISTRY__H__
