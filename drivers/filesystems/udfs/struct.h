////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
/*************************************************************************
*
* File: struct.h
*
* Module: UDF File System Driver (Kernel mode execution only)
*
* Description:
*   This file contains structure definitions for the UDF file system
*   driver. Note that all structures are prefixed with the letters
*   "UDF". The structures are all aligned using normal alignment
*   used by the compiler (typically quad-word aligned).
*
*************************************************************************/

#ifndef _UDF_STRUCTURES_H_
#define _UDF_STRUCTURES_H_


/**************************************************************************
    some useful definitions
**************************************************************************/

#include "Include/platform.h"


/**************************************************************************
 include udf related structures *here* (because we need definition of Fcb)
**************************************************************************/
#include "udf_info/udf_rel.h"

struct IRP_CONTEXT_LITE;
struct IO_CONTEXT;

/**************************************************************************
    each structure has a unique "node type" or signature associated with it
**************************************************************************/

using NODE_TYPE_CODE = USHORT;
using NODE_BYTE_SIZE = CSHORT;

#define UDF_NODE_TYPE_UNDEFINED             ((NODE_TYPE_CODE)0x0000)
#define UDF_NODE_TYPE_OBJECT_NAME           ((NODE_TYPE_CODE)0xba01)
#define UDF_NODE_TYPE_CCB                   ((NODE_TYPE_CODE)0xba02)
#define UDF_NODE_TYPE_FCB                   ((NODE_TYPE_CODE)0xba03)
#define UDF_NODE_TYPE_VCB                   ((NODE_TYPE_CODE)0xba04)
#define UDF_NODE_TYPE_IRP_CONTEXT           ((NODE_TYPE_CODE)0xba05)
#define UDF_NODE_TYPE_GLOBAL_DATA           ((NODE_TYPE_CODE)0xba06)
#define UDF_NODE_TYPE_FILTER_DEVOBJ         ((NODE_TYPE_CODE)0xba07)
#define UDF_NODE_TYPE_UDFFS_DEVOBJ          ((NODE_TYPE_CODE)0xba08)
#define UDF_NODE_TYPE_IRP_CONTEXT_LITE      ((NODE_TYPE_CODE)0xba09)
#define UDF_NODE_TYPE_UDFFS_DRVOBJ          ((NODE_TYPE_CODE)0xba0a)

/**************************************************************************
    every structure has a node type, and a node size associated with it.
    The node type serves as a signature field. The size is used for
    consistency checking ...
**************************************************************************/
struct UDFIdentifier {
    NODE_TYPE_CODE NodeTypeCode;           // a 16 bit identifier for the structure
    NODE_BYTE_SIZE NodeByteSize;           // computed as sizeof(structure)
};

static_assert(sizeof(UDFIdentifier) == offsetof(FSRTL_ADVANCED_FCB_HEADER, Flags),
    "UDFIdentifier size mismatch with NodeTypeCode and NodeByteSize in FSRTL_ADVANCED_FCB_HEADER");

/**************************************************************************
    Every open on-disk object must have a name associated with it
    This name has two components:
    (a) the path-name (prefix) that leads to this on-disk object
    (b) the name of the object itself
    Note that with multiply linked objects, a single object might be
    associated with more than one name structure.
    This UDF FSD does not correctly support multiply linked objects.

    This structure must be quad-word aligned because it is zone allocated.
**************************************************************************/
struct UDFObjectName {
    UDFIdentifier                       NodeIdentifier;
    uint32                              ObjectNameFlags;
    // an absolute pathname of the object is stored below
    UNICODE_STRING                      ObjectName;
};
using PtrUDFObjectName = UDFObjectName*;

#define     UDF_OBJ_NAME_NOT_FROM_ZONE               (0x80000000)

/**************************************************************************
    Each file open instance is represented by a context control block.
    For each successful create/open request; a file object and a CCB will
    be created.
    For open operations performed internally by the FSD, there may not
    exist file objects; but a CCB will definitely be created.

    This structure must be quad-word aligned because it is zone allocated.
**************************************************************************/
struct CCB {
    UDFIdentifier                       NodeIdentifier;
    // ptr to the associated FCB
    FCB*                                Fcb;
    // all CCB structures for a FCB are linked together
    LIST_ENTRY                          NextCCB;
    // each CCB is associated with a file object
    PFILE_OBJECT                        FileObject;
    // flags (see below) associated with this CCB
    uint32                              CCBFlags;
    // current index in directory is required sometimes
    ULONG                               CurrentIndex;
    // if this CCB represents a directory object open, we may
    //  need to maintain a search pattern
    PUNICODE_STRING                     DirectorySearchPattern;
    HASH_ENTRY                          hashes;
    ULONG                               TreeLength;
    // Acces rights previously granted to caller's thread
    ACCESS_MASK                         PreviouslyGrantedAccess;
};
using PCCB = CCB*;

/**************************************************************************
    the following CCBFlags values are relevant. These flag
    values are bit fields; therefore we can test whether
    a bit position is set (1) or not set (0).
**************************************************************************/

// some on-disk file/directories are opened by UDF itself
//  as opposed to being opened on behalf of a user process
#define UDF_CCB_OPENED_BY_UDF                   (0x00000001)
// the file object specified synchronous access at create/open time.
//  this implies that UDF must maintain the current byte offset
#define UDF_CCB_OPENED_FOR_SYNC_ACCESS          (0x00000002)
// file object specified sequential access for this file
#define UDF_CCB_OPENED_FOR_SEQ_ACCESS           (0x00000004)
// the CCB has had an IRP_MJ_CLEANUP issued on it. we must
//  no longer allow the file object / CCB to be used in I/O requests.
#define UDF_CCB_CLEANED                         (0x00000008)
// if we were invoked via the fast i/o path to perform file i/o;
//  we should set the CCB access/modification time at cleanup
#define UDF_CCB_ACCESSED                        (0x00000010)
#define UDF_CCB_MODIFIED                        (0x00000020)
// if an application process set the file date time, we must
//  honor that request and *not* overwrite the values at cleanup
#define UDF_CCB_ACCESS_TIME_SET                 (0x00000040)
#define UDF_CCB_MODIFY_TIME_SET                 (0x00000080)
#define UDF_CCB_CREATE_TIME_SET                 (0x00000100)
#define UDF_CCB_WRITE_TIME_SET                  (0x00000200)
#define UDF_CCB_ATTRIBUTES_SET                  (0x00020000)

#define UDF_CCB_CASE_SENSETIVE                  (0x00000400)
#define UDF_CCB_DELETE_ON_CLOSE                 (0x00000800)
#define UDF_CCB_FLAG_DISMOUNT_ON_CLOSE          (0x00040000)

// this CCB was allocated for a "volume open" operation
#define UDF_CCB_VOLUME_OPEN                     (0x00001000)
#define UDF_CCB_MATCH_ALL                       (0x00002000)
#define UDF_CCB_WILDCARD_PRESENT                (0x00004000)
#define UDF_CCB_CAN_BE_8_DOT_3                  (0x00008000)
#define UDF_CCB_READ_ONLY                       (0x00010000)
//#define UDF_CCB_ATTRIBUTES_SET                (0x00020000) // see above

#define UDF_CCB_FLUSHED                         (0x20000000)
#define UDF_CCB_VALID                           (0x40000000)
#define UDF_CCB_NOT_FROM_ZONE                   (0x80000000)


/**************************************************************************
    each open file/directory/volume is represented by a file control block.

    Each FCB can logically be divided into two:
    (a) a structure that must have a field of type FSRTL_COMMON_FCB_HEADER
         as the first field in the structure.
         This portion should also contain other structures/resources required
         by the NT Cache Manager
         We will call this structure the "NT Required" FCB. Note that this
         portion of the FCB must be allocated from non-paged pool.
    (b) the remainder of the FCB is dependent upon the particular FSD
         requirements.
         This portion of the FCB could possibly be allocated from paged
         memory, though in the UDF FSD, it will always be allocated
         from non-paged pool.

    FCB structures are protected by the MainResource as well as the
    PagingIoResource. Of course, if the FSD implementation requires
    it, we can associate other syncronization structures with the
    FCB.

    These structures must be quad-word aligned because they are zone-allocated.
**************************************************************************/

struct UDFNTRequiredFCB {

    union {
        UDFIdentifier                   NodeIdentifier;
        FSRTL_ADVANCED_FCB_HEADER       Header;

    };

    SECTION_OBJECT_POINTERS             SectionObject;
    FILE_LOCK                           FileLock;
    ERESOURCE                           MainResource;
    ERESOURCE                           PagingIoResource;
    FAST_MUTEX                          AdvancedFCBHeaderMutex;
    // we will maintain some time information here to make our life easier
    LARGE_INTEGER                       CreationTime;
    LARGE_INTEGER                       LastAccessTime;
    LARGE_INTEGER                       LastWriteTime;
    LARGE_INTEGER                       ChangeTime;
    // NT requires that a file system maintain and honor the various
    //  SHARE_ACCESS modes ...
    SHARE_ACCESS                        FCBShareAccess;
    // This counter is used to prevent unexpected structure releases
    ULONG                               CommonRefCount;
    ULONG                               NtReqFCBFlags;
    // to identify the lazy writer thread(s) we will grab and store
    //  the thread id here when a request to acquire resource(s)
    //  arrives ..
    uint32                              LazyWriterThreadID;
    UCHAR                               AcqSectionCount;
    UCHAR                               AcqFlushCount;
};
using PtrUDFNTRequiredFCB = UDFNTRequiredFCB*;

#define     UDF_NTREQ_FCB_DELETED       (0x00000004)
#define     UDF_NTREQ_FCB_MODIFIED      (0x00000008)
#define     UDF_NTREQ_FCB_VALID         (0x40000000)

/**************************************************************************/

#define UDF_FCB_MT NonPagedPool

/***************************************************/
/*****************  W A R N I N G  *****************/
/***************************************************/

/***************************************************/
/*      DO NOT FORGET TO UPDATE VCB's HEADER !     */
/***************************************************/

struct FCB : UDFNTRequiredFCB {

    // UDF related data
    PUDF_FILE_INFO                      FileInfo;
    // this FCB belongs to some mounted logical volume
    struct VCB*      Vcb;
    // to be able to access all open file(s) for a volume, we will
    //  link all FCB structures for a logical volume together
    LIST_ENTRY                          NextFCB;
    // some state information for the FCB is maintained using the
    //  flags field
    uint32                              FCBFlags;
    // all CCB's for this particular FCB are linked off the following
    //  list head.
    LIST_ENTRY                          NextCCB;
    // whenever a file stream has a create/open operation performed,
    //  the Reference count below is incremented AND the OpenHandle count
    //  below is also incremented.
    //  When an IRP_MJ_CLEANUP is received, the OpenHandle count below
    //  is decremented.
    //  When an IRP_MJ_CLOSE is received, the Reference count below is
    //  decremented.
    //  When the Reference count goes down to zero, the FCB can be de-allocated.
    //  Note that a zero Reference count implies a zero OpenHandle count.
    //  But when we have mapped data, we can receive no IRP_MJ_CLOSE
    //  In this case OpenHandleCount may reach zero, but ReferenceCount may
    //  be non-zero.
    uint32                              ReferenceCount;
    uint32                              OpenHandleCount;
    uint32                              CachedOpenHandleCount;
    // for the UDF fsd, there exists a 1-1 correspondence between a
    //  full object pathname and a FCB
    PtrUDFObjectName                    FCBName;
    ERESOURCE                           CcbListResource;

    FCB* ParentFcb;
    // Pointer to IrpContextLite in delayed queue.
    IRP_CONTEXT_LITE* IrpContextLite;
    uint32                              CcbCount;
};
using PFCB = FCB*;

/**************************************************************************
    the following FCBFlags values are relevant. These flag
    values are bit fields; therefore we can test whether
    a bit position is set (1) or not set (0).
**************************************************************************/
#define     UDF_FCB_VALID                               (0x00000002)

#define     UDF_FCB_PAGE_FILE                           (0x00000004)
#define     UDF_FCB_DIRECTORY                           (0x00000008)
#define     UDF_FCB_ROOT_DIRECTORY                      (0x00000010)
#define     UDF_FCB_WRITE_THROUGH                       (0x00000020)
#define     UDF_FCB_MAPPED                              (0x00000040)
#define     UDF_FCB_FAST_IO_READ_IN_PROGESS             (0x00000080)
#define     UDF_FCB_FAST_IO_WRITE_IN_PROGESS            (0x00000100)
#define     UDF_FCB_DELETE_ON_CLOSE                     (0x00000200)
#define     UDF_FCB_MODIFIED                            (0x00000400)
#define     UDF_FCB_ACCESSED                            (0x00000800)
#define     UDF_FCB_READ_ONLY                           (0x00001000)
#define     UDF_FCB_DELAY_CLOSE                         (0x00002000)
#define     UDF_FCB_DELETED                             (0x00004000)

#define     UDF_FCB_INITIALIZED_CCB_LIST_RESOURCE       (0x00008000)
#define     UDF_FCB_POSTED_RENAME                       (0x00010000)

#define     UDF_FCB_DELETE_PARENT                       (0x10000000)
#define     UDF_FCB_NOT_FROM_ZONE                       (0x80000000)

/**************************************************************************
    A logical volume is represented with the following structure.
    This structure is allocated as part of the device extension
    for a device object that this FSD will create, to represent
    the mounted logical volume.

**************************************************************************/

#define _BROWSE_UDF_

enum UDFFSD_MEDIA_TYPE {
    MediaUnknown = 0,
    MediaHdd,
    MediaCdr,
    MediaCdrw,
    MediaCdrom,
    MediaZip,
    MediaFloppy,
    MediaDvdr,
    MediaDvdrw
};

enum VCB_CONDITION {

    VcbNotMounted = 0,
    VcbMountInProgress,
    VcbMounted,
    VcbInvalid,
    VcbDismountInProgress
};

struct VCB : FCB {

    // Condition flag for the Vcb.
    VCB_CONDITION VcbCondition;

    uint32                              VCBOpenCount;
    uint32                              VCBOpenCountRO;
    uint32                              VCBHandleCount;
    // a resource to protect the fields contained within the VCB
    ERESOURCE                           FcbListResource;
    ERESOURCE                           FlushResource;
    // each VCB is accessible off a global linked list
    LIST_ENTRY                          NextVCB;
    // each VCB points to a VPB structure created by the NT I/O Manager
    PVPB                                Vpb;
    // we will maintain a global list of IRP's that are pending
    //  because of a directory notify request.
    LIST_ENTRY                          NextNotifyIRP;
    // the above list is protected only by the mutex declared below
    PNOTIFY_SYNC                        NotifyIRPMutex;
    // for each mounted volume, we create a device object. Here then
    //  is a back pointer to that device object
    PDEVICE_OBJECT                      VCBDeviceObject;
    // We also retain a pointer to the physical device object on which we
    // have mounted ourselves. The I/O Manager passes us a pointer to this
    // device object when requesting a mount operation.
    PDEVICE_OBJECT                      TargetDeviceObject;
    PCWSTR                               DefaultRegName;
    // the volume structure contains a pointer to the root directory FCB
    FCB* RootDirFCB;
    // the complete name of the user visible drive letter we serve
    PUCHAR                              PtrVolumePath;
    // Pointer to a stream file object created for the volume information
    // to be more easily read from secondary storage (with the support of
    // the NT Cache Manager).
/*    PFILE_OBJECT                        PtrStreamFileObject;
    // Required to use the Cache Manager.
*/
    struct _FILE_SYSTEM_STATISTICS* Statistics;
    // Volume lock file object - used in Lock/Unlock routines
    ULONG                               VolumeLockPID;
    PFILE_OBJECT                        VolumeLockFileObject;
    DEVICE_TYPE                         FsDeviceType;

    //  The following field tells how many requests for this volume have
    //  either been enqueued to ExWorker threads or are currently being
    //  serviced by ExWorker threads.  If the number goes above
    //  a certain threshold, put the request on the overflow queue to be
    //  executed later.
    ULONG PostedRequestCount;
    ULONG ThreadsPerCpu;
    //  The following field indicates the number of IRP's waiting
    //  to be serviced in the overflow queue.
    ULONG OverflowQueueCount;
    //  The following field contains the queue header of the overflow queue.
    //  The Overflow queue is a list of IRP's linked via the IRP's ListEntry
    //  field.
    LIST_ENTRY OverflowQueue;
    //  The following spinlock protects access to all the above fields.
    KSPIN_LOCK OverflowQueueSpinLock;
    ULONG StopOverflowQueue;
    ULONG SystemCacheGran;

    //---------------
    //
    //---------------

    ULONG           BM_FlushTime;
    ULONG           BM_FlushPriod;
    ULONG           Tree_FlushTime;
    ULONG           Tree_FlushPriod;
    ULONG           SkipCountLimit;
    ULONG           SkipEjectCountLimit;

    // File Id cache
    struct _UDFFileIDCacheItem* FileIdCache;
    ULONG           FileIdCount;
    //
    ULONG           MediaLockCount;

    BOOLEAN         IsVolumeJustMounted;

    // FS size cache
    LONGLONG        TotalAllocUnits;
    LONGLONG        FreeAllocUnits;
    LONGLONG        EstimatedFreeUnits;

    // a resource to protect the fields contained within the VCB
    ERESOURCE                           VCBResource;
    ERESOURCE                           BitMapResource1;
    ERESOURCE                           FileIdResource;
    ERESOURCE                           DlocResource;
    ERESOURCE                           DlocResource2;
    ERESOURCE                           PreallocResource;
    ERESOURCE                           IoResource;

    //---------------
    // Physical media parameters
    //---------------

    ULONG           BlockSize;
    ULONG           BlockSizeBits;
    ULONG           WriteBlockSize;
    ULONG           LBlockSize;
    ULONG           LBlockSizeBits;
    ULONG           LB2B_Bits;
    // Number of last session
    ULONG           LastSession;
    ULONG           FirstTrackNum;
    ULONG           FirstTrackNumLastSes;
    ULONG           LastTrackNum;
    // First & Last LBA of the last session
    ULONG           FirstLBA;
    ULONG           FirstLBALastSes;
    ULONG           LastLBA;
    // Last writable LBA
    ULONG           LastPossibleLBA;
    // First writable LBA
    ULONG           NWA;
    // sector type map
    struct _UDFTrackMap* TrackMap;
    ULONG           LastModifiedTrack;
    ULONG           LastReadTrack;
    ULONG           CdrwBufferSize;
    ULONG           CdrwBufferSizeCounter;
    uint32          SavedFeatures;
    // OPC info
//    PCHAR           OPC_buffer;
    UCHAR           OPCNum;
    BOOLEAN         OPCDone;
    UCHAR           MediaType;
    UCHAR           MediaClassEx;

    UCHAR           PhErasable;
    UCHAR           PhDiskType;
    UCHAR           PhMediaCapFlags;

    UCHAR           MRWStatus;
    BOOLEAN         BlankCD;
    UCHAR           Reserved;

    ULONG           PhSerialNumber;

    BOOLEAN         CDR_Mode;
    BOOLEAN         DVD_Mode;

#define SYNC_CACHE_RECOVERY_NONE     0
#define SYNC_CACHE_RECOVERY_ATTEMPT  1
#define SYNC_CACHE_RECOVERY_RETRY    2

    UCHAR           SyncCacheState;

    // W-cache
    W_CACHE         FastCache;
    ULONG           WCacheMaxFrames;
    ULONG           WCacheMaxBlocks;
    ULONG           WCacheBlocksPerFrameSh;
    ULONG           WCacheFramesToKeepFree;

    PCHAR           ZBuffer;
    PCHAR           fZBuffer;
    ULONG           fZBufferSize;

    ULONG           IoErrorCounter;
    // Media change count (equal to the same field in CDFS VCB)
    ULONG           MediaChangeCount;

#define INCREMENTAL_SEEK_NONE        0
#define INCREMENTAL_SEEK_WORKAROUND  1
#define INCREMENTAL_SEEK_DONE        2

    UCHAR           IncrementalSeekState;

    BOOLEAN         VerifyOnWrite;
    BOOLEAN         DoNotCompareBeforeWrite;
    BOOLEAN         CacheChainedIo;

    ULONG           MountPhErrorCount;

    // a set of flags that might mean something useful
    uint32          VCBFlags;
    BOOLEAN         FP_disc;

    //---------------
    // UDF related data
    //---------------

    // Anchors LBA
#define MAX_ANCHOR_LOCATIONS 11
    ULONG           Anchor[MAX_ANCHOR_LOCATIONS];
    ULONG           BadSeqLoc[MAX_ANCHOR_LOCATIONS * 2];
    OSSTATUS        BadSeqStatus[MAX_ANCHOR_LOCATIONS * 2];
    ULONG           BadSeqLocIndex;
    // Volume label
    UNICODE_STRING  VolIdent;
    // Volume creation time
    int64           VolCreationTime;
    // Root & SystemStream lb_addr
    lb_addr         RootLbAddr;
    lb_addr         SysStreamLbAddr;
    // Number of partition
    ULONG           PartitionMaps;
    // Pointer to partition structures
    PUDFPartMap     Partitions;
    LogicalVolIntegrityDesc* LVid;
    uint32          IntegrityType;
    uint32          origIntegrityType;
    extent_ad       LVid_loc;
    ULONG           SerialNumber;
    // on-disk structure version control
    uint16          CurrentUDFRev;
    uint16          minUDFReadRev;
    uint16          minUDFWriteRev;
    uint16          maxUDFWriteRev;
    // file/dir counters for Mac OS
    uint32          numFiles;
    uint32          numDirs;
    // VAT
    uint32          InitVatCount;
    uint32          VatCount;
    uint32* Vat;
    uint32          VatPartNdx;
    PUDF_FILE_INFO  VatFileInfo;
    // sparing table
    ULONG           SparingCountFree;
    ULONG           SparingCount;
    ULONG           SparingBlockSize;
    struct _SparingEntry* SparingTable;
#define MAX_SPARING_TABLE_LOCATIONS 32
    uint32          SparingTableLoc[MAX_SPARING_TABLE_LOCATIONS];
    uint32          SparingTableCount;
    uint32          SparingTableLength;
    uint32          SparingTableModified;
    // free space bitmap
    ULONG           FSBM_ByteCount;
    // the following 2 fields are equal to NTIFS's RTL_BITMAP structure
    ULONG           FSBM_BitCount;
    PCHAR           FSBM_Bitmap;     // 0 - free, 1 - used
#ifdef UDF_TRACK_ONDISK_ALLOCATION_OWNERS
    PULONG          FSBM_Bitmap_owners; // 0 - free
    // -1 - used by unknown
    // other - owner's FE location
#ifdef UDF_TRACK_FS_STRUCTURES
    PEXTENT_MAP     FsStructMap;
    PULONG          FE_link_counts; // 0 - free
#endif //UDF_TRACK_FS_STRUCTURES
#endif //UDF_TRACK_ONDISK_ALLOCATION_OWNERS

    PCHAR           FSBM_OldBitmap;  // 0 - free, 1 - used
    ULONG           BitmapModified;

    PCHAR           ZSBM_Bitmap;     // 0 - data, 1 - zero-filleld

    PCHAR           BSBM_Bitmap;     // 0 - normal, 1 - bad-block

    // pointers to Volume Descriptor Sequences
    ULONG VDS1;
    ULONG VDS1_Len;
    ULONG VDS2;
    ULONG VDS2_Len;

    ULONG           Modified;

    // System Stream Dir
    PUDF_FILE_INFO  SysSDirFileInfo;
    // Non-alloc space
    PUDF_FILE_INFO  NonAllocFileInfo;
    // Unique ID Mapping
    PUDF_FILE_INFO  UniqueIDMapFileInfo;
    // Saved location of Primary Vol Descr (used for setting Label)
    UDF_VDS_RECORD  PVolDescAddr;
    UDF_VDS_RECORD  PVolDescAddr2;
    // NSR flags
    uint32          NSRDesc;
    // File Id cache
    ULONGLONG       NextUniqueId;
    // FE location cache
    PUDF_DATALOC_INDEX DlocList;
    ULONG           DlocCount;
    // FS compatibility
    USHORT          DefaultAllocMode; // Default alloc mode (from registry)
    BOOLEAN         UseExtendedFE;
    BOOLEAN         LowFreeSpace;
    UDFFSD_MEDIA_TYPE MediaTypeEx;
    ULONG           DefaultUID;
    ULONG           DefaultGID;
    ULONG           DefaultAttr;      // Default file attributes (NT-style)


    UCHAR           PartitialDamagedVolumeAction;
    BOOLEAN         NoFreeRelocationSpaceVolumeAction;
    BOOLEAN         WriteSecurity;
    BOOLEAN         FlushMedia;
    BOOLEAN         ForgetVolume;
    UCHAR           Reserved5[3];

    //
    ULONG           FECharge;
    ULONG           FEChargeSDir;
    ULONG           PackDirThreshold;
    ULONG           SparseThreshold;  // in blocks

    PUDF_ALLOCATION_CACHE_ITEM    FEChargeCache;
    ULONG                         FEChargeCacheMaxSize;

    PUDF_ALLOCATION_CACHE_ITEM    PreallocCache;
    ULONG                         PreallocCacheMaxSize;

    UDF_VERIFY_CTX  VerifyCtx;

    PUCHAR          Cfg;
    ULONG           CfgLength;
    ULONG           CfgVersion;

    uint32          CompatFlags;
    UCHAR           ShowBlankCd;

    // Preallocated VPB for swapout, so we are not forced to consider
    // must succeed pool.
    PVPB SwapVpb;
};

using PVCB = VCB*;

// One for root
#define         UDF_RESIDUAL_REFERENCE              (2)

// input flush flags
#define         UDF_FLUSH_FLAGS_BREAKABLE           (0x00000001)
// see also udf_rel.h
#define         UDF_FLUSH_FLAGS_LITE                (0x80000000)
// output flush flags
#define         UDF_FLUSH_FLAGS_INTERRUPTED         (0x00000001)

#define         UDF_MAX_BG_WRITERS                  16

typedef struct _FILTER_DEV_EXTENSION {
    UDFIdentifier   NodeIdentifier;
    PFILE_OBJECT    fileObject;
    PDEVICE_OBJECT  lowerFSDeviceObject;
} FILTER_DEV_EXTENSION, *PFILTER_DEV_EXTENSION;

typedef struct _UDFFS_DEV_EXTENSION {
    UDFIdentifier   NodeIdentifier;
} UDFFS_DEV_EXTENSION, *PUDFFS_DEV_EXTENSION;
/**************************************************************************
    The IRP context encapsulates the current request. This structure is
    used in the "common" dispatch routines invoked either directly in
    the context of the original requestor, or indirectly in the context
    of a system worker thread.
**************************************************************************/
typedef struct _IRP_CONTEXT {
    UDFIdentifier                   NodeIdentifier;
    ULONG                           Flags;
    // copied from the IRP
    UCHAR                           MajorFunction;
    // copied from the IRP
    UCHAR                           MinorFunction;
    // to queue this IRP for asynchronous processing
    WORK_QUEUE_ITEM                 WorkQueueItem;
    // the IRP for which this context structure was created
    PIRP                            Irp;
    // the target of the request (obtained from the IRP)
    PDEVICE_OBJECT                  TargetDeviceObject;
    // if an exception occurs, we will store the code here
    NTSTATUS                        SavedExceptionCode;
    // For queued close operation we save Fcb
    FCB*                            Fcb;
    ULONG                           TreeLength;
    IO_CONTEXT* IoContext;
    VCB*      Vcb;
} IRP_CONTEXT;
typedef IRP_CONTEXT *PIRP_CONTEXT;

#define IRP_CONTEXT_FLAG_ON_STACK               (0x00000001)
#define IRP_CONTEXT_FLAG_MORE_PROCESSING        (0x00000002)
#define IRP_CONTEXT_FLAG_WAIT                   (0x00000004)
#define IRP_CONTEXT_FLAG_FORCE_POST             (0x00000008)
#define IRP_CONTEXT_FLAG_TOP_LEVEL              (0x00000010)
#define IRP_CONTEXT_FLAG_TOP_LEVEL_UDFS         (0x00000020)
#define IRP_CONTEXT_FLAG_IN_FSP                 (0x00000040)
#define IRP_CONTEXT_FLAG_IN_TEARDOWN            (0x00000080)
#define IRP_CONTEXT_FLAG_ALLOC_IO               (0x00000100)
#define IRP_CONTEXT_FLAG_DISABLE_POPUPS         (0x00000200)
#define IRP_CONTEXT_FLAG_DEFERRED_WRITE         (0x00000400)
#define IRP_CONTEXT_FLAG_WRITE_THROUGH          (0x00020000)

#define UDF_IRP_CONTEXT_NOT_TOP_LEVEL           (0x10000000)
#define UDF_IRP_CONTEXT_FLUSH_REQUIRED          (0x20000000)
#define UDF_IRP_CONTEXT_FLUSH2_REQUIRED         (0x40000000)

/**************************************************************************
    Following structure is used to queue a request to the delayed close queue.
    This structure should be the minimum block allocation size.
**************************************************************************/
struct IRP_CONTEXT_LITE {
    UDFIdentifier                   NodeIdentifier;
    //  Fcb for the file object being closed.
    FCB*                            Fcb;
    //  List entry to attach to delayed close queue.
    LIST_ENTRY                      DelayedCloseLinks;
    //  User reference count for the file object being closed.
    //ULONG                           UserReference;
    //  Real device object.  This represents the physical device closest to the media.
    PDEVICE_OBJECT                  RealDevice;
    ULONG                           TreeLength;
};
using PIRP_CONTEXT_LITE = IRP_CONTEXT_LITE*;

/**************************************************************************
    we will store all of our global variables in one structure.
    Global variables are not specific to any mounted volume BUT
    by definition are required for successful operation of the
    FSD implementation.
**************************************************************************/
typedef struct _UDFData {

    UDFIdentifier               NodeIdentifier;
    // the fields in this list are protected by the following resource
    ERESOURCE                   GlobalDataResource;
    // each driver has a driver object created for it by the NT I/O Mgr.
    //  we are no exception to this rule.
    PDRIVER_OBJECT              DriverObject;
    // we will create a device object for our FSD as well ...
    //  Although not really required, it helps if a helper application
    //  writen by us wishes to send us control information via
    //  IOCTL requests ...
    PDEVICE_OBJECT              UDFDeviceObject_CD;
    PDEVICE_OBJECT              UDFDeviceObject_HDD;
    // we will keep a list of all logical volumes for our UDF FSD
    LIST_ENTRY                  VCBQueue;
    // the NT Cache Manager, the I/O Manager and we will conspire
    //  to bypass IRP usage using the function pointers contained
    //  in the following structure
    FAST_IO_DISPATCH            UDFFastIoDispatch;
    // The NT Cache Manager uses the following call backs to ensure
    //  correct locking hierarchy is maintained
    CACHE_MANAGER_CALLBACKS     CacheMgrCallBacks;

    // Our lookaside lists.
    NPAGED_LOOKASIDE_LIST IrpContextLookasideList;
    NPAGED_LOOKASIDE_LIST ObjectNameLookasideList;
    NPAGED_LOOKASIDE_LIST NonPagedFcbLookasideList;

    PAGED_LOOKASIDE_LIST CcbLookasideList;

    // delayed close support
    ERESOURCE                   DelayedCloseResource;
    ULONG                       MaxDelayedCloseCount;
    ULONG                       DelayedCloseCount;
    ULONG                       MinDelayedCloseCount;
    ULONG                       MaxDirDelayedCloseCount;
    ULONG                       DirDelayedCloseCount;
    ULONG                       MinDirDelayedCloseCount;
    LIST_ENTRY                  DelayedCloseQueue;
    LIST_ENTRY                  DirDelayedCloseQueue;
    WORK_QUEUE_ITEM             CloseItem;
    WORK_QUEUE_ITEM             LicenseKeyItem;
    BOOLEAN                     LicenseKeyItemStarted;
    BOOLEAN                     FspCloseActive;
    BOOLEAN                     ReduceDelayedClose;
    BOOLEAN                     ReduceDirDelayedClose;

    ULONG                       CPU_Count;
    LARGE_INTEGER               UDFLargeZero;

    // mount event (for udf gui app)
    PKEVENT                     MountEvent;

    UNICODE_STRING              SavedRegPath;
    UNICODE_STRING              UnicodeStrRoot;
    UNICODE_STRING              UnicodeStrSDir;
    UNICODE_STRING              AclName;

    ULONG                       WCacheMaxFrames;
    ULONG                       WCacheMaxBlocks;
    ULONG                       WCacheBlocksPerFrameSh;
    ULONG                       WCacheFramesToKeepFree;

    // some state information is maintained in the flags field
    uint32                      UDFFlags;

    PVOID                       AutoFormatCount;

} UDFData, * PtrUDFData;

#define TAG_IRP_CONTEXT         'cidU'
#define TAG_OBJECT_NAME         'nodU'
#define TAG_FCB_NONPAGED        'nfdU'
#define TAG_CCB                 'ccdU'
#define TAG_VPB                 'pvdU'

// some valid flags for the VCB
#define         UDF_VCB_FLAGS_VOLUME_LOCKED         (0x00000002)
#define         UDF_VCB_FLAGS_SHUTDOWN              (0x00000008)
#define         UDF_VCB_FLAGS_VOLUME_READ_ONLY      (0x00000010)

#define         UDF_VCB_FLAGS_VCB_INITIALIZED       (0x00000020)
#define         UDF_VCB_FLAGS_NO_SYNC_CACHE         (0x00000080)
#define         UDF_VCB_FLAGS_REMOVABLE_MEDIA       (0x00000100)
#define         UDF_VCB_FLAGS_MEDIA_LOCKED          (0x00000200)
#define         UDF_VCB_SKIP_EJECT_CHECK            (0x00000400)
#define         UDF_VCB_LAST_WRITE                  (0x00001000)
#define         UDF_VCB_FLAGS_TRACKMAP              (0x00002000)
#define         UDF_VCB_ASSUME_ALL_USED             (0x00004000)

#define         UDF_VCB_FLAGS_RAW_DISK              (0x00040000)
#define         UDF_VCB_FLAGS_USE_STD               (0x00080000)

#define         UDF_VCB_FLAGS_NO_DELAYED_CLOSE      (0x00200000)
#define         UDF_VCB_FLAGS_MEDIA_READ_ONLY       (0x00400000)

#define         UDF_VCB_FLAGS_FLUSH_BREAK_REQ       (0x01000000)
#define         UDF_VCB_FLAGS_EJECT_REQ             (0x02000000)
#define         UDF_VCB_FLAGS_FORCE_SYNC_CACHE      (0x04000000)

#define         UDF_VCB_FLAGS_UNSAFE_IOCTL          (0x10000000)
#define         UDF_VCB_FLAGS_DEAD                  (0x20000000)  // device unexpectedly disappeared


// flags for FS Interface Compatibility
#define         UDF_VCB_IC_UPDATE_ACCESS_TIME          (0x00000001)
#define         UDF_VCB_IC_UPDATE_MODIFY_TIME          (0x00000002)
#define         UDF_VCB_IC_UPDATE_ATTR_TIME            (0x00000004)
#define         UDF_VCB_IC_UPDATE_ARCH_BIT             (0x00000008)
#define         UDF_VCB_IC_UPDATE_DIR_WRITE            (0x00000010)
#define         UDF_VCB_IC_UPDATE_DIR_READ             (0x00000020)
#define         UDF_VCB_IC_WRITE_IN_RO_DIR             (0x00000040)
#define         UDF_VCB_IC_UPDATE_UCHG_DIR_ACCESS_TIME (0x00000080)
#define         UDF_VCB_IC_W2K_COMPAT_ALLOC_DESCS      (0x00000100)
#define         UDF_VCB_IC_HW_RO                       (0x00000200)
#define         UDF_VCB_IC_OS_NATIVE_DOS_NAME          (0x00000400)
#define         UDF_VCB_IC_FORCE_WRITE_THROUGH         (0x00000800)
#define         UDF_VCB_IC_FORCE_HW_RO                 (0x00001000)
#define         UDF_VCB_IC_IGNORE_SEQUENTIAL_IO        (0x00002000)
#define         UDF_VCB_IC_NO_SYNCCACHE_AFTER_WRITE    (0x00004000)
#define         UDF_VCB_IC_BAD_RW_SEEK                 (0x00008000)
#define         UDF_VCB_IC_FP_ADDR_PROBLEM             (0x00010000)
#define         UDF_VCB_IC_MRW_ADDR_PROBLEM            (0x00020000)
#define         UDF_VCB_IC_BAD_DVD_LAST_LBA            (0x00040000)
#define         UDF_VCB_IC_SYNCCACHE_BEFORE_READ       (0x00080000)
#define         UDF_VCB_IC_INSTANT_COMPAT_ALLOC_DESCS  (0x00100000)
#define         UDF_VCB_IC_SOFT_RO                     (0x00200000)

#define         UDF_VCB_IC_DIRTY_RO                    (0x04000000)
#define         UDF_VCB_IC_W2K_COMPAT_VLABEL           (0x08000000)
#define         UDF_VCB_IC_CACHE_BAD_VDS               (0x10000000)
#define         UDF_VCB_IC_WAIT_CD_SPINUP              (0x20000000)
#define         UDF_VCB_IC_SHOW_BLANK_CD               (0x40000000)
#define         UDF_VCB_IC_ADAPTEC_NONALLOC_COMPAT     (0x80000000)

// valid flag values for the global data structure
#define     UDF_DATA_FLAGS_RESOURCE_INITIALIZED     (0x00000001)
#define     UDF_DATA_FLAGS_ZONES_INITIALIZED        (0x00000002)
#define     UDF_DATA_FLAGS_BEING_UNLOADED           (0x00000004)

#define FILE_ID_CACHE_GRANULARITY 16
#define DLOC_LIST_GRANULARITY 16

// Some defines
#define UDFIsDvdMedia(Vcb)       (Vcb->DVD_Mode)
#define UDFIsWriteParamsReq(Vcb) (Vcb->WriteParamsReq && !Vcb->DVD_Mode)

//  Define the file system statistics struct.  Vcb->Statistics points to an
//  array of these (one per processor) and they must be 64 byte aligned to
//  prevent cache line tearing.
typedef struct _FILE_SYSTEM_STATISTICS {
    //  This contains the actual data.
    FILESYSTEM_STATISTICS Common;
    FAT_STATISTICS Fat;
    //  Pad this structure to a multiple of 64 bytes.
    UCHAR Pad[64-(sizeof(FILESYSTEM_STATISTICS)+sizeof(FAT_STATISTICS))%64];
} FILE_SYSTEM_STATISTICS, *PFILE_SYSTEM_STATISTICS;

//
typedef struct _UDFFileIDCacheItem {
    LONGLONG Id;
    UNICODE_STRING FullName;
    BOOLEAN CaseSens;
} UDFFileIDCacheItem, *PUDFFileIDCacheItem;

#define DIRTY_PAGE_LIMIT   32

#define MAXIMUM_NUMBER_TRACKS_LARGE 0xAA

typedef struct _CDROM_TOC_LARGE {

    //
    // Header
    //

    UCHAR Length[2];  // add two bytes for this field
    UCHAR FirstTrack;
    UCHAR LastTrack;

    //
    // Track data
    //

    TRACK_DATA TrackData[MAXIMUM_NUMBER_TRACKS_LARGE];

} CDROM_TOC_LARGE;

#endif /* _UDF_STRUCTURES_H_ */ // has this file been included?

