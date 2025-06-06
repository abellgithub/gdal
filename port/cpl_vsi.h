/******************************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 * Purpose:  Include file defining Virtual File System (VSI) functions, a
 *           layer over POSIX file and other system services.
 *
 ******************************************************************************
 * Copyright (c) 1998, Frank Warmerdam
 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef CPL_VSI_H_INCLUDED
#define CPL_VSI_H_INCLUDED

#include "cpl_port.h"
#include "cpl_progress.h"

#include <stdbool.h>

/**
 * \file cpl_vsi.h
 *
 * Standard C Covers
 *
 * The VSI (Virtual System Interface) functions are intended to be hookable
 * aliases for Standard C I/O, memory allocation and other system functions.
 * They are intended to allow virtualization of disk I/O so that non file data
 * sources can be made to appear as files, and so that additional error trapping
 * and reporting can be interested.  The memory access API is aliased
 * so that special application memory management services can be used.
 *
 * It is intended that each of these functions retains exactly the same
 * calling pattern as the original Standard C functions they relate to.
 * This means we don't have to provide custom documentation, and also means
 * that the default implementation is very simple.
 */

/* -------------------------------------------------------------------- */
/*      We need access to ``struct stat''.                              */
/* -------------------------------------------------------------------- */

/* Unix */
#if !defined(_WIN32)
#include <unistd.h>
#endif

/* Windows */
#include <sys/stat.h>

CPL_C_START

/*! @cond Doxygen_Suppress */
#ifdef ENABLE_EXPERIMENTAL_CPL_WARN_UNUSED_RESULT
#define EXPERIMENTAL_CPL_WARN_UNUSED_RESULT CPL_WARN_UNUSED_RESULT
#else
#define EXPERIMENTAL_CPL_WARN_UNUSED_RESULT
#endif
/*! @endcond */

/* ==================================================================== */
/*      stdio file access functions.  These do not support large       */
/*      files, and do not go through the virtualization API.           */
/* ==================================================================== */

/*! @cond Doxygen_Suppress */

FILE CPL_DLL *VSIFOpen(const char *, const char *) CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFClose(FILE *);
int CPL_DLL VSIFSeek(FILE *, long, int) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
long CPL_DLL VSIFTell(FILE *) CPL_WARN_UNUSED_RESULT;
void CPL_DLL VSIRewind(FILE *);
void CPL_DLL VSIFFlush(FILE *);

size_t CPL_DLL VSIFRead(void *, size_t, size_t,
                        FILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
size_t CPL_DLL VSIFWrite(const void *, size_t, size_t,
                         FILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
char CPL_DLL *VSIFGets(char *, int, FILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFPuts(const char *, FILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFPrintf(FILE *, CPL_FORMAT_STRING(const char *),
                       ...) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT
    CPL_PRINT_FUNC_FORMAT(2, 3);

int CPL_DLL VSIFGetc(FILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFPutc(int, FILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIUngetc(int, FILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFEof(FILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;

/*! @endcond */

/* ==================================================================== */
/*      VSIStat() related.                                              */
/* ==================================================================== */

/*! @cond Doxygen_Suppress */
typedef struct stat VSIStatBuf;
int CPL_DLL VSIStat(const char *, VSIStatBuf *) CPL_WARN_UNUSED_RESULT;
/*! @endcond */

#ifdef _WIN32
#define VSI_ISLNK(x) (0) /* N/A on Windows */
#define VSI_ISREG(x) ((x)&S_IFREG)
#define VSI_ISDIR(x) ((x)&S_IFDIR)
#define VSI_ISCHR(x) ((x)&S_IFCHR)
#define VSI_ISBLK(x) (0) /* N/A on Windows */
#else
/** Test if the file is a symbolic link */
#define VSI_ISLNK(x) S_ISLNK(x)
/** Test if the file is a regular file */
#define VSI_ISREG(x) S_ISREG(x)
/** Test if the file is a directory */
#define VSI_ISDIR(x) S_ISDIR(x)
/*! @cond Doxygen_Suppress */
#define VSI_ISCHR(x) S_ISCHR(x)
#define VSI_ISBLK(x) S_ISBLK(x)
/*! @endcond */
#endif

/* ==================================================================== */
/*      64bit stdio file access functions.  If we have a big size       */
/*      defined, then provide prototypes for the large file API,        */
/*      otherwise redefine to use the regular api.                      */
/* ==================================================================== */

/** Type for a file offset */
typedef GUIntBig vsi_l_offset;
/** Maximum value for a file offset */
#define VSI_L_OFFSET_MAX GUINTBIG_MAX

/** Opaque type for a FILE that implements the VSIVirtualHandle API */
typedef struct VSIVirtualHandle VSILFILE;

VSILFILE CPL_DLL *VSIFOpenL(const char *, const char *) CPL_WARN_UNUSED_RESULT;
VSILFILE CPL_DLL *VSIFOpenExL(const char *, const char *,
                              int) CPL_WARN_UNUSED_RESULT;
VSILFILE CPL_DLL *VSIFOpenEx2L(const char *, const char *, int,
                               CSLConstList) CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFCloseL(VSILFILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFSeekL(VSILFILE *, vsi_l_offset,
                      int) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
vsi_l_offset CPL_DLL VSIFTellL(VSILFILE *) CPL_WARN_UNUSED_RESULT;
void CPL_DLL VSIRewindL(VSILFILE *);
size_t CPL_DLL VSIFReadL(void *, size_t, size_t,
                         VSILFILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFReadMultiRangeL(int nRanges, void **ppData,
                                const vsi_l_offset *panOffsets,
                                const size_t *panSizes,
                                VSILFILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
size_t CPL_DLL VSIFWriteL(const void *, size_t, size_t,
                          VSILFILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
void CPL_DLL VSIFClearErrL(VSILFILE *);
int CPL_DLL VSIFErrorL(VSILFILE *) CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFEofL(VSILFILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFTruncateL(VSILFILE *,
                          vsi_l_offset) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFFlushL(VSILFILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;
int CPL_DLL VSIFPrintfL(VSILFILE *, CPL_FORMAT_STRING(const char *),
                        ...) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT
    CPL_PRINT_FUNC_FORMAT(2, 3);
int CPL_DLL VSIFPutcL(int, VSILFILE *) EXPERIMENTAL_CPL_WARN_UNUSED_RESULT;

/** Range status */
typedef enum
{
    VSI_RANGE_STATUS_UNKNOWN, /**< Unknown */
    VSI_RANGE_STATUS_DATA,    /**< Data present */
    VSI_RANGE_STATUS_HOLE     /**< Hole */
} VSIRangeStatus;

VSIRangeStatus CPL_DLL VSIFGetRangeStatusL(VSILFILE *fp, vsi_l_offset nStart,
                                           vsi_l_offset nLength);

int CPL_DLL VSIIngestFile(VSILFILE *fp, const char *pszFilename,
                          GByte **ppabyRet, vsi_l_offset *pnSize,
                          GIntBig nMaxSize) CPL_WARN_UNUSED_RESULT;

int CPL_DLL VSIOverwriteFile(VSILFILE *fpTarget, const char *pszSourceFilename)
    CPL_WARN_UNUSED_RESULT;

#if defined(VSI_STAT64_T)
/** Type for VSIStatL() */
typedef struct VSI_STAT64_T VSIStatBufL;
#else
/** Type for VSIStatL() */
#define VSIStatBufL VSIStatBuf
#endif

int CPL_DLL VSIStatL(const char *, VSIStatBufL *) CPL_WARN_UNUSED_RESULT;

/** Flag provided to VSIStatExL() to test if the file exists */
#define VSI_STAT_EXISTS_FLAG 0x1
/** Flag provided to VSIStatExL() to query the nature (file/dir) of the file */
#define VSI_STAT_NATURE_FLAG 0x2
/** Flag provided to VSIStatExL() to query the file size */
#define VSI_STAT_SIZE_FLAG 0x4
/** Flag provided to VSIStatExL() to issue a VSIError in case of failure */
#define VSI_STAT_SET_ERROR_FLAG 0x8
/** Flag provided to VSIStatExL() to only use already cached results.
 * @since GDAL 3.4
 */
#define VSI_STAT_CACHE_ONLY 0x10

int CPL_DLL VSIStatExL(const char *pszFilename, VSIStatBufL *psStatBuf,
                       int nFlags) CPL_WARN_UNUSED_RESULT;

int CPL_DLL VSIIsCaseSensitiveFS(const char *pszFilename);

int CPL_DLL VSISupportsSparseFiles(const char *pszPath);

bool CPL_DLL VSIIsLocal(const char *pszPath);

char CPL_DLL *VSIGetCanonicalFilename(const char *pszPath);

bool CPL_DLL VSISupportsSequentialWrite(const char *pszPath,
                                        bool bAllowLocalTempFile);

bool CPL_DLL VSISupportsRandomWrite(const char *pszPath,
                                    bool bAllowLocalTempFile);

int CPL_DLL VSIHasOptimizedReadMultiRange(const char *pszPath);

const char CPL_DLL *VSIGetActualURL(const char *pszFilename);

char CPL_DLL *VSIGetSignedURL(const char *pszFilename,
                              CSLConstList papszOptions);

const char CPL_DLL *VSIGetFileSystemOptions(const char *pszFilename);

char CPL_DLL **VSIGetFileSystemsPrefixes(void);

void CPL_DLL *VSIFGetNativeFileDescriptorL(VSILFILE *);

char CPL_DLL **
VSIGetFileMetadata(const char *pszFilename, const char *pszDomain,
                   CSLConstList papszOptions) CPL_WARN_UNUSED_RESULT;

int CPL_DLL VSISetFileMetadata(const char *pszFilename,
                               CSLConstList papszMetadata,
                               const char *pszDomain,
                               CSLConstList papszOptions);

void CPL_DLL VSISetPathSpecificOption(const char *pszPathPrefix,
                                      const char *pszKey, const char *pszValue);
void CPL_DLL VSIClearPathSpecificOptions(const char *pszPathPrefix);
const char CPL_DLL *VSIGetPathSpecificOption(const char *pszPath,
                                             const char *pszKey,
                                             const char *pszDefault);

void CPL_DLL VSISetCredential(const char *pszPathPrefix, const char *pszKey,
                              const char *pszValue)
    /*! @cond Doxygen_Suppress */
    CPL_WARN_DEPRECATED("Use VSISetPathSpecificOption instead")
    /*! @endcond */
    ;
void CPL_DLL VSIClearCredentials(const char *pszPathPrefix)
    /*! @cond Doxygen_Suppress */
    CPL_WARN_DEPRECATED("Use VSIClearPathSpecificOptions instead")
    /*! @endcond */
    ;
const char CPL_DLL *VSIGetCredential(const char *pszPath, const char *pszKey,
                                     const char *pszDefault)
    /*! @cond Doxygen_Suppress */
    CPL_WARN_DEPRECATED("Use VSIGetPathSpecificOption instead")
    /*! @endcond */
    ;

/* ==================================================================== */
/*      Memory allocation                                               */
/* ==================================================================== */

void CPL_DLL *VSICalloc(size_t, size_t) CPL_WARN_UNUSED_RESULT;
void CPL_DLL *VSIMalloc(size_t) CPL_WARN_UNUSED_RESULT;
void CPL_DLL VSIFree(void *);
void CPL_DLL *VSIRealloc(void *, size_t) CPL_WARN_UNUSED_RESULT;
char CPL_DLL *VSIStrdup(const char *) CPL_WARN_UNUSED_RESULT;

#if defined(__cplusplus) && defined(GDAL_COMPILATION)
extern "C++"
{
    /*! @cond Doxygen_Suppress */
    struct CPL_DLL VSIFreeReleaser
    {
        void operator()(void *p) const
        {
            VSIFree(p);
        }
    };

    /*! @endcond */
}
#endif

void CPL_DLL *VSIMallocAligned(size_t nAlignment,
                               size_t nSize) CPL_WARN_UNUSED_RESULT;
void CPL_DLL *VSIMallocAlignedAuto(size_t nSize) CPL_WARN_UNUSED_RESULT;
void CPL_DLL VSIFreeAligned(void *ptr);

void CPL_DLL *VSIMallocAlignedAutoVerbose(size_t nSize, const char *pszFile,
                                          int nLine) CPL_WARN_UNUSED_RESULT;
/** VSIMallocAlignedAutoVerbose() with FILE and LINE reporting */
#define VSI_MALLOC_ALIGNED_AUTO_VERBOSE(size)                                  \
    VSIMallocAlignedAutoVerbose(size, __FILE__, __LINE__)

/**
 VSIMalloc2 allocates (nSize1 * nSize2) bytes.
 In case of overflow of the multiplication, or if memory allocation fails, a
 NULL pointer is returned and a CE_Failure error is raised with CPLError().
 If nSize1 == 0 || nSize2 == 0, a NULL pointer will also be returned.
 CPLFree() or VSIFree() can be used to free memory allocated by this function.
*/
void CPL_DLL *VSIMalloc2(size_t nSize1, size_t nSize2) CPL_WARN_UNUSED_RESULT;

/**
 VSIMalloc3 allocates (nSize1 * nSize2 * nSize3) bytes.
 In case of overflow of the multiplication, or if memory allocation fails, a
 NULL pointer is returned and a CE_Failure error is raised with CPLError().
 If nSize1 == 0 || nSize2 == 0 || nSize3 == 0, a NULL pointer will also be
 returned. CPLFree() or VSIFree() can be used to free memory allocated by this
 function.
*/
void CPL_DLL *VSIMalloc3(size_t nSize1, size_t nSize2,
                         size_t nSize3) CPL_WARN_UNUSED_RESULT;

/** VSIMallocVerbose */
void CPL_DLL *VSIMallocVerbose(size_t nSize, const char *pszFile,
                               int nLine) CPL_WARN_UNUSED_RESULT;
/** VSI_MALLOC_VERBOSE */
#define VSI_MALLOC_VERBOSE(size) VSIMallocVerbose(size, __FILE__, __LINE__)

/** VSIMalloc2Verbose */
void CPL_DLL *VSIMalloc2Verbose(size_t nSize1, size_t nSize2,
                                const char *pszFile,
                                int nLine) CPL_WARN_UNUSED_RESULT;
/** VSI_MALLOC2_VERBOSE */
#define VSI_MALLOC2_VERBOSE(nSize1, nSize2)                                    \
    VSIMalloc2Verbose(nSize1, nSize2, __FILE__, __LINE__)

/** VSIMalloc3Verbose */
void CPL_DLL *VSIMalloc3Verbose(size_t nSize1, size_t nSize2, size_t nSize3,
                                const char *pszFile,
                                int nLine) CPL_WARN_UNUSED_RESULT;
/** VSI_MALLOC3_VERBOSE */
#define VSI_MALLOC3_VERBOSE(nSize1, nSize2, nSize3)                            \
    VSIMalloc3Verbose(nSize1, nSize2, nSize3, __FILE__, __LINE__)

/** VSICallocVerbose */
void CPL_DLL *VSICallocVerbose(size_t nCount, size_t nSize, const char *pszFile,
                               int nLine) CPL_WARN_UNUSED_RESULT;
/** VSI_CALLOC_VERBOSE */
#define VSI_CALLOC_VERBOSE(nCount, nSize)                                      \
    VSICallocVerbose(nCount, nSize, __FILE__, __LINE__)

/** VSIReallocVerbose */
void CPL_DLL *VSIReallocVerbose(void *pOldPtr, size_t nNewSize,
                                const char *pszFile,
                                int nLine) CPL_WARN_UNUSED_RESULT;
/** VSI_REALLOC_VERBOSE */
#define VSI_REALLOC_VERBOSE(pOldPtr, nNewSize)                                 \
    VSIReallocVerbose(pOldPtr, nNewSize, __FILE__, __LINE__)

/** VSIStrdupVerbose */
char CPL_DLL *VSIStrdupVerbose(const char *pszStr, const char *pszFile,
                               int nLine) CPL_WARN_UNUSED_RESULT;
/** VSI_STRDUP_VERBOSE */
#define VSI_STRDUP_VERBOSE(pszStr) VSIStrdupVerbose(pszStr, __FILE__, __LINE__)

GIntBig CPL_DLL CPLGetPhysicalRAM(void);
GIntBig CPL_DLL CPLGetUsablePhysicalRAM(void);

/* ==================================================================== */
/*      Other...                                                        */
/* ==================================================================== */

/** Alias of VSIReadDir() */
#define CPLReadDir VSIReadDir
char CPL_DLL **VSIReadDir(const char *);
char CPL_DLL **VSIReadDirRecursive(const char *pszPath);
char CPL_DLL **VSIReadDirEx(const char *pszPath, int nMaxFiles);
char CPL_DLL **VSISiblingFiles(const char *pszPath);
char CPL_DLL **VSIGlob(const char *pszPattern, const char *const *papszOptions,
                       GDALProgressFunc pProgressFunc, void *pProgressData);

const char CPL_DLL *VSIGetDirectorySeparator(const char *pszPath);

/** Opaque type for a directory iterator */
typedef struct VSIDIR VSIDIR;

VSIDIR CPL_DLL *VSIOpenDir(const char *pszPath, int nRecurseDepth,
                           const char *const *papszOptions);

/*! @cond Doxygen_Suppress */
typedef struct VSIDIREntry VSIDIREntry;

/*! @endcond */

/** Directory entry. */
struct VSIDIREntry
{
    /** Filename */
    char *pszName;
    /** File mode. See VSI_ISREG() / VSI_ISDIR() */
    int nMode;
    /** File size */
    vsi_l_offset nSize;
    /** Last modification time (seconds since 1970/01/01) */
    GIntBig nMTime;
    /** Whether nMode is known: 0 = unknown, 1 = known. */
    char bModeKnown;
    /** Whether nSize is known: 0 = unknown, 1 = known. */
    char bSizeKnown;
    /** Whether nMTime is known: 0 = unknown, 1 = known. */
    char bMTimeKnown;
    /** NULL-terminated list of extra properties. */
    char **papszExtra;

#if defined(__cplusplus) && !defined(CPL_SUPRESS_CPLUSPLUS)
    /*! @cond Doxygen_Suppress */
    VSIDIREntry();
    ~VSIDIREntry();
    VSIDIREntry(const VSIDIREntry &);
    VSIDIREntry &operator=(VSIDIREntry &) = delete;
/*! @endcond */
#endif
};

const VSIDIREntry CPL_DLL *VSIGetNextDirEntry(VSIDIR *dir);
void CPL_DLL VSICloseDir(VSIDIR *dir);

int CPL_DLL VSIMkdir(const char *pszPathname, long mode);
int CPL_DLL VSIMkdirRecursive(const char *pszPathname, long mode);
int CPL_DLL VSIRmdir(const char *pszDirname);
int CPL_DLL VSIRmdirRecursive(const char *pszDirname);
int CPL_DLL VSIUnlink(const char *pszFilename);
int CPL_DLL *VSIUnlinkBatch(CSLConstList papszFiles);
int CPL_DLL VSIRename(const char *oldpath, const char *newpath);
int CPL_DLL VSIMove(const char *oldpath, const char *newpath,
                    const char *const *papszOptions,
                    GDALProgressFunc pProgressFunc, void *pProgressData);
int CPL_DLL VSICopyFile(const char *pszSource, const char *pszTarget,
                        VSILFILE *fpSource, vsi_l_offset nSourceSize,
                        const char *const *papszOptions,
                        GDALProgressFunc pProgressFunc, void *pProgressData);
int CPL_DLL VSICopyFileRestartable(const char *pszSource, const char *pszTarget,
                                   const char *pszInputPayload,
                                   char **ppszOutputPayload,
                                   const char *const *papszOptions,
                                   GDALProgressFunc pProgressFunc,
                                   void *pProgressData);
int CPL_DLL VSISync(const char *pszSource, const char *pszTarget,
                    const char *const *papszOptions,
                    GDALProgressFunc pProgressFunc, void *pProgressData,
                    char ***ppapszOutputs);

int CPL_DLL VSIMultipartUploadGetCapabilities(
    const char *pszFilename, int *pbNonSequentialUploadSupported,
    int *pbParallelUploadSupported, int *pbAbortSupported,
    size_t *pnMinPartSize, size_t *pnMaxPartSize, int *pnMaxPartCount);

char CPL_DLL *VSIMultipartUploadStart(const char *pszFilename,
                                      CSLConstList papszOptions);
char CPL_DLL *VSIMultipartUploadAddPart(const char *pszFilename,
                                        const char *pszUploadId,
                                        int nPartNumber,
                                        vsi_l_offset nFileOffset,
                                        const void *pData, size_t nDataLength,
                                        CSLConstList papszOptions);
int CPL_DLL VSIMultipartUploadEnd(const char *pszFilename,
                                  const char *pszUploadId, size_t nPartIdsCount,
                                  const char *const *apszPartIds,
                                  vsi_l_offset nTotalSize,
                                  CSLConstList papszOptions);
int CPL_DLL VSIMultipartUploadAbort(const char *pszFilename,
                                    const char *pszUploadId,
                                    CSLConstList papszOptions);

int CPL_DLL VSIAbortPendingUploads(const char *pszFilename);

char CPL_DLL *VSIStrerror(int);
GIntBig CPL_DLL VSIGetDiskFreeSpace(const char *pszDirname);

void CPL_DLL VSINetworkStatsReset(void);
char CPL_DLL *VSINetworkStatsGetAsSerializedJSON(char **papszOptions);

/* ==================================================================== */
/*      Install special file access handlers.                           */
/* ==================================================================== */
void CPL_DLL VSIInstallMemFileHandler(void);
/*! @cond Doxygen_Suppress */
void CPL_DLL VSIInstallLargeFileHandler(void);
/*! @endcond */
void CPL_DLL VSIInstallSubFileHandler(void);
void VSIInstallCurlFileHandler(void);
void CPL_DLL VSICurlClearCache(void);
void CPL_DLL VSICurlPartialClearCache(const char *pszFilenamePrefix);
void VSIInstallCurlStreamingFileHandler(void);
void VSIInstallS3FileHandler(void);
void VSIInstallS3StreamingFileHandler(void);
void VSIInstallGSFileHandler(void);
void VSIInstallGSStreamingFileHandler(void);
void VSIInstallAzureFileHandler(void);
void VSIInstallAzureStreamingFileHandler(void);
void VSIInstallADLSFileHandler(void);
void VSIInstallOSSFileHandler(void);
void VSIInstallOSSStreamingFileHandler(void);
void VSIInstallSwiftFileHandler(void);
void VSIInstallSwiftStreamingFileHandler(void);
void VSIInstall7zFileHandler(void);   /* No reason to export that */
void VSIInstallRarFileHandler(void);  /* No reason to export that */
void VSIInstallGZipFileHandler(void); /* No reason to export that */
void VSIInstallZipFileHandler(void);  /* No reason to export that */
void VSIInstallStdinHandler(void);    /* No reason to export that */
void VSIInstallHdfsHandler(void);     /* No reason to export that */
void VSIInstallWebHdfsHandler(void);  /* No reason to export that */
void VSIInstallStdoutHandler(void);   /* No reason to export that */
void CPL_DLL VSIInstallSparseFileHandler(void);
void VSIInstallTarFileHandler(void);    /* No reason to export that */
void VSIInstallCachedFileHandler(void); /* No reason to export that */
void CPL_DLL VSIInstallCryptFileHandler(void);
void CPL_DLL VSISetCryptKey(const GByte *pabyKey, int nKeySize);
/*! @cond Doxygen_Suppress */
void CPL_DLL VSICleanupFileManager(void);
/*! @endcond */

bool CPL_DLL VSIDuplicateFileSystemHandler(const char *pszSourceFSName,
                                           const char *pszNewFSName);

VSILFILE CPL_DLL *
VSIFileFromMemBuffer(const char *pszFilename, GByte *pabyData,
                     vsi_l_offset nDataLength,
                     int bTakeOwnership) CPL_WARN_UNUSED_RESULT;
GByte CPL_DLL *VSIGetMemFileBuffer(const char *pszFilename,
                                   vsi_l_offset *pnDataLength,
                                   int bUnlinkAndSeize);

const char CPL_DLL *VSIMemGenerateHiddenFilename(const char *pszFilename);

/** Callback used by VSIStdoutSetRedirection() */
typedef size_t (*VSIWriteFunction)(const void *ptr, size_t size, size_t nmemb,
                                   FILE *stream);
void CPL_DLL VSIStdoutSetRedirection(VSIWriteFunction pFct, FILE *stream);

/**
 * Return information about a handle. Optional (driver dependent)
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginStatCallback)(void *pUserData,
                                               const char *pszFilename,
                                               VSIStatBufL *pStatBuf,
                                               int nFlags);
/**
 * Remove handle by name. Optional
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginUnlinkCallback)(void *pUserData,
                                                 const char *pszFilename);
/**
 * Rename handle. Optional
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginRenameCallback)(void *pUserData,
                                                 const char *oldpath,
                                                 const char *newpath);
/**
 * Create Directory. Optional
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginMkdirCallback)(void *pUserData,
                                                const char *pszDirname,
                                                long nMode);
/**
 *  Delete Directory. Optional
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginRmdirCallback)(void *pUserData,
                                                const char *pszDirname);
/**
 * List directory content. Optional
 * @since GDAL 3.0
 */
typedef char **(*VSIFilesystemPluginReadDirCallback)(void *pUserData,
                                                     const char *pszDirname,
                                                     int nMaxFiles);
/**
 * List related files. Must return NULL if unknown, or a list of relative
 * filenames that can be opened along the main file. If no other file than
 * pszFilename needs to be opened, return static_cast<char**>
 * (CPLCalloc(1,sizeof(char*)));
 *
 * Optional
 * @since GDAL 3.2
 */
typedef char **(*VSIFilesystemPluginSiblingFilesCallback)(
    void *pUserData, const char *pszDirname);
/**
 * Open a handle. Mandatory. Returns an opaque pointer that will be used in
 * subsequent file I/O calls. Should return null and/or set errno if the handle
 * does not exist or the access mode is incorrect.
 * @since GDAL 3.0
 */
typedef void *(*VSIFilesystemPluginOpenCallback)(void *pUserData,
                                                 const char *pszFilename,
                                                 const char *pszAccess);
/**
 * Return current position in handle. Mandatory
 * @since GDAL 3.0
 */
typedef vsi_l_offset (*VSIFilesystemPluginTellCallback)(void *pFile);
/**
 * Seek to position in handle. Mandatory except for write only handles
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginSeekCallback)(void *pFile,
                                               vsi_l_offset nOffset,
                                               int nWhence);
/**
 * Read data from current position, returns the number of blocks correctly read.
 * Mandatory except for write only handles
 * @since GDAL 3.0
 */
typedef size_t (*VSIFilesystemPluginReadCallback)(void *pFile, void *pBuffer,
                                                  size_t nSize, size_t nCount);
/**
 * Read from multiple offsets. Optional, will be replaced by multiple calls to
 * Read() if not provided
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginReadMultiRangeCallback)(
    void *pFile, int nRanges, void **ppData, const vsi_l_offset *panOffsets,
    const size_t *panSizes);
/**
 * Get empty ranges. Optional
 * @since GDAL 3.0
 */
typedef VSIRangeStatus (*VSIFilesystemPluginGetRangeStatusCallback)(
    void *pFile, vsi_l_offset nOffset, vsi_l_offset nLength);
/**
 * Has end of file been reached. Mandatory? for read handles.
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginEofCallback)(void *pFile);
/**
 * Write bytes at current offset. Mandatory for writable handles
 * @since GDAL 3.0
 */
typedef size_t (*VSIFilesystemPluginWriteCallback)(void *pFile,
                                                   const void *pBuffer,
                                                   size_t nSize, size_t nCount);
/**
 * Sync written bytes. Optional
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginFlushCallback)(void *pFile);
/**
 * Truncate handle. Mandatory (driver dependent?) for write handles
 */
typedef int (*VSIFilesystemPluginTruncateCallback)(void *pFile,
                                                   vsi_l_offset nNewSize);
/**
 * Close file handle. Optional
 * @since GDAL 3.0
 */
typedef int (*VSIFilesystemPluginCloseCallback)(void *pFile);

/**
 * This optional method is called when code plans to access soon one or several
 * ranges in a file. Some file systems may be able to use this hint to
 * for example asynchronously start such requests.
 *
 * Offsets may be given in a non-increasing order, and may potentially
 * overlap.
 *
 * @param pFile File handle.
 * @param nRanges Size of the panOffsets and panSizes arrays.
 * @param panOffsets Array containing the start offset of each range.
 * @param panSizes Array containing the size (in bytes) of each range.
 * @since GDAL 3.7
 */
typedef void (*VSIFilesystemPluginAdviseReadCallback)(
    void *pFile, int nRanges, const vsi_l_offset *panOffsets,
    const size_t *panSizes);

/**
 * Has a read error (non end-of-file related) has occurred?
 * @since GDAL 3.10
 */
typedef int (*VSIFilesystemPluginErrorCallback)(void *pFile);

/**
 * Clear error and end-of-file flags.
 * @since GDAL 3.10
 */
typedef void (*VSIFilesystemPluginClearErrCallback)(void *pFile);

/**
 * struct containing callbacks to used by the handler.
 * (rw), (r), (w) or () at the end indicate whether the given callback is
 * mandatory for reading and or writing handlers. A (?) indicates that the
 * callback might be mandatory for certain drivers only.
 * @since GDAL 3.0
 */
typedef struct
{
    /**
     * Optional opaque pointer passed back to filemanager callbacks (e.g. open,
     * stat, rmdir)
     */
    void *pUserData;
    VSIFilesystemPluginStatCallback stat;     /**< stat handle by name (rw)*/
    VSIFilesystemPluginUnlinkCallback unlink; /**< unlink handle by name ()*/
    VSIFilesystemPluginRenameCallback rename; /**< rename handle ()*/
    VSIFilesystemPluginMkdirCallback mkdir;   /**< make directory ()*/
    VSIFilesystemPluginRmdirCallback rmdir;   /**< remove directory ()*/
    VSIFilesystemPluginReadDirCallback
        read_dir;                         /**< list directory content (r?)*/
    VSIFilesystemPluginOpenCallback open; /**< open handle by name (rw) */
    VSIFilesystemPluginTellCallback
        tell; /**< get current position of handle (rw) */
    VSIFilesystemPluginSeekCallback
        seek; /**< set current position of handle (rw) */
    VSIFilesystemPluginReadCallback read; /**< read from current position (r) */
    VSIFilesystemPluginReadMultiRangeCallback
        read_multi_range; /**< read multiple blocks ()*/
    VSIFilesystemPluginGetRangeStatusCallback
        get_range_status; /**< get range status () */
    VSIFilesystemPluginEofCallback
        eof; /**< has end of file been reached (r?) */
    VSIFilesystemPluginWriteCallback
        write; /**< write bytes to current position (w) */
    VSIFilesystemPluginFlushCallback flush;       /**< sync bytes (w) */
    VSIFilesystemPluginTruncateCallback truncate; /**< truncate handle (w?) */
    VSIFilesystemPluginCloseCallback close;       /**< close handle  (rw) */
    size_t nBufferSize; /**< buffer small reads (makes handler read only) */
    size_t nCacheSize;  /**< max mem to use per file when buffering */
    VSIFilesystemPluginSiblingFilesCallback
        sibling_files; /**< list related files*/

    /** The following optional member has been added in GDAL 3.7: */
    VSIFilesystemPluginAdviseReadCallback advise_read; /**< AdviseRead() */

    VSIFilesystemPluginErrorCallback error; /**< has read error occurred (r) */
    VSIFilesystemPluginClearErrCallback clear_err; /**< clear error flags(r) */
    /*
        Callbacks are defined as a struct allocated by a call to
       VSIAllocFilesystemPluginCallbacksStruct in order to try to maintain ABI
       stability when eventually adding a new member. Any callbacks added to
       this struct SHOULD be added to the END of this struct
    */
} VSIFilesystemPluginCallbacksStruct;

/**
 * return a VSIFilesystemPluginCallbacksStruct to be populated at runtime with
 * handler callbacks
 * @since GDAL 3.0
 */
VSIFilesystemPluginCallbacksStruct CPL_DLL *
VSIAllocFilesystemPluginCallbacksStruct(void);

/**
 * free resources allocated by VSIAllocFilesystemPluginCallbacksStruct
 * @since GDAL 3.0
 */
void CPL_DLL VSIFreeFilesystemPluginCallbacksStruct(
    VSIFilesystemPluginCallbacksStruct *poCb);

/**
 * register a handler on the given prefix. All IO on datasets opened with the
 * filename /prefix/xxxxxx will go through these callbacks. pszPrefix must begin
 * and end with a '/'
 * @since GDAL 3.0
 */
int CPL_DLL VSIInstallPluginHandler(
    const char *pszPrefix, const VSIFilesystemPluginCallbacksStruct *poCb);

/**
 * Unregister a handler previously installed with VSIInstallPluginHandler() on
 * the given prefix.
 * Note: it is generally unsafe to remove a handler while there are still file
 * handles opened that are managed by that handler. It is the responsibility of
 * the caller to ensure that it calls this function in a situation where it is
 * safe to do so.
 * @since GDAL 3.9
 */
int CPL_DLL VSIRemovePluginHandler(const char *pszPrefix);

/* ==================================================================== */
/*      Time querying.                                                  */
/* ==================================================================== */

/*! @cond Doxygen_Suppress */
unsigned long CPL_DLL VSITime(unsigned long *);
const char CPL_DLL *VSICTime(unsigned long);
struct tm CPL_DLL *VSIGMTime(const time_t *pnTime, struct tm *poBrokenTime);
struct tm CPL_DLL *VSILocalTime(const time_t *pnTime, struct tm *poBrokenTime);
/*! @endcond */

/*! @cond Doxygen_Suppress */
/* -------------------------------------------------------------------- */
/*      the following can be turned on for detailed logging of          */
/*      almost all IO calls.                                            */
/* -------------------------------------------------------------------- */
#ifdef VSI_DEBUG

#ifndef DEBUG
#define DEBUG
#endif

#include "cpl_error.h"

#define VSIDebug4(f, a1, a2, a3, a4) CPLDebug("VSI", f, a1, a2, a3, a4);
#define VSIDebug3(f, a1, a2, a3) CPLDebug("VSI", f, a1, a2, a3);
#define VSIDebug2(f, a1, a2) CPLDebug("VSI", f, a1, a2);
#define VSIDebug1(f, a1) CPLDebug("VSI", f, a1);
#else
#define VSIDebug4(f, a1, a2, a3, a4)                                           \
    {                                                                          \
    }
#define VSIDebug3(f, a1, a2, a3)                                               \
    {                                                                          \
    }
#define VSIDebug2(f, a1, a2)                                                   \
    {                                                                          \
    }
#define VSIDebug1(f, a1)                                                       \
    {                                                                          \
    }
#endif
/*! @endcond */

CPL_C_END

#endif /* ndef CPL_VSI_H_INCLUDED */
