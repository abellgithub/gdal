/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  Test block cache under multi-threading
 * Author:   Even Rouault, <even dot rouault at spatialys dot com>
 *
 ******************************************************************************
 * Copyright (c) 2015, Even Rouault <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef DEBUG
#define DEBUG
#endif

#include "cpl_multiproc.h"
#include "gdal_priv.h"

#include <cstdlib>
#include <vector>

#include "gtest_include.h"

extern int global_argc;
extern char **global_argv;

namespace
{

CPLLock *psLock = nullptr;

static void Usage()
{
    printf("Usage: testblockcache [-threads X] [-loops X] [-max_requests X] "
           "[-strategy random|line|block]\n");
    printf("                      [-migrate] [ filename |\n");
    printf("                       [[-xsize val] [-ysize val] [-bands val] "
           "[-co key=value]*\n");
    printf("                       [[-memdriver] | [-ondisk]] [-check]] ]\n");
    exit(1);
}

int nLoops = 1;
const char *pszDataset = nullptr;
int bCheck = FALSE;

typedef enum
{
    STRATEGY_RANDOM,
    STRATEGY_LINE,
    STRATEGY_BLOCK,
} Strategy;

typedef struct _Request Request;

struct _Request
{
    int nXOff, nYOff, nXWin, nYWin;
    int nBands;
    Request *psNext;
};

typedef struct _Resource Resource;

struct _Resource
{
    GDALDataset *poDS;
    void *pBuffer;
    Resource *psNext;
    Resource *psPrev;
};

typedef struct
{
    GDALDataset *poDS;
    Request *psRequestList;
    int nBufferSize;
} ThreadDescription;

static Request *psGlobalRequestList = nullptr;
static Resource *psGlobalResourceList = nullptr;
static Resource *psGlobalResourceLast = nullptr;

/* according to rand() man page, POSIX.1-2001 proposes the following
 * implementation */
/* RAND_MAX assumed to be 32767 */
#define MYRAND_MAX 32767

static int myrand_r(unsigned long *pseed)
{
    *pseed = *pseed * 1103515245 + 12345;
    return ((unsigned)((*pseed / 65536UL) % (MYRAND_MAX + 1)));
}

static void Check(GByte *pBuffer, int nXSize, int nYSize, int nBands, int nXOff,
                  int nYOff, int nXWin, int nYWin)
{
    for (int iBand = 0; iBand < nBands; iBand++)
    {
        for (int iY = 0; iY < nYWin; iY++)
        {
            for (int iX = 0; iX < nXWin; iX++)
            {
                unsigned long seed = iBand * nXSize * nYSize +
                                     (iY + nYOff) * nXSize + iX + nXOff;
                GByte expected = (GByte)(myrand_r(&seed) & 0xff);
                EXPECT_EQ(pBuffer[iBand * nXWin * nYWin + iY * nXWin + iX],
                          expected);
            }
        }
    }
}

static void ReadRaster(GDALDataset *poDS, int nXSize, int nYSize, int nBands,
                       GByte *pBuffer, int nXOff, int nYOff, int nXWin,
                       int nYWin)
{
    CPL_IGNORE_RET_VAL(poDS->RasterIO(GF_Read, nXOff, nYOff, nXWin, nYWin,
                                      pBuffer, nXWin, nYWin, GDT_Byte, nBands,
                                      nullptr, 0, 0, 0
#ifdef GDAL_COMPILATION
                                      ,
                                      nullptr
#endif
                                      ));
    if (bCheck)
    {
        Check(pBuffer, nXSize, nYSize, nBands, nXOff, nYOff, nXWin, nYWin);
    }
}

static void AddRequest(Request *&psRequestList, Request *&psRequestLast,
                       int nXOff, int nYOff, int nXWin, int nYWin, int nBands)
{
    Request *psRequest = (Request *)CPLMalloc(sizeof(Request));
    psRequest->nXOff = nXOff;
    psRequest->nYOff = nYOff;
    psRequest->nXWin = nXWin;
    psRequest->nYWin = nYWin;
    psRequest->nBands = nBands;
    if (psRequestLast)
        psRequestLast->psNext = psRequest;
    else
        psRequestList = psRequest;
    psRequestLast = psRequest;
    psRequest->psNext = nullptr;
}

static Request *GetNextRequest(Request *&psRequestList)
{
    if (psLock)
        CPLAcquireLock(psLock);
    Request *psRet = psRequestList;
    if (psRequestList)
    {
        psRequestList = psRequestList->psNext;
        psRet->psNext = nullptr;
    }
    if (psLock)
        CPLReleaseLock(psLock);
    return psRet;
}

static Resource *AcquireFirstResource()
{
    if (psLock)
        CPLAcquireLock(psLock);
    Resource *psRet = psGlobalResourceList;
    psGlobalResourceList = psGlobalResourceList->psNext;
    if (psGlobalResourceList)
        psGlobalResourceList->psPrev = nullptr;
    else
        psGlobalResourceLast = nullptr;
    psRet->psNext = nullptr;
    CPLAssert(psRet->psPrev == nullptr);
    if (psLock)
        CPLReleaseLock(psLock);
    return psRet;
}

static void PutResourceAtEnd(Resource *psResource)
{
    if (psLock)
        CPLAcquireLock(psLock);
    psResource->psPrev = psGlobalResourceLast;
    psResource->psNext = nullptr;
    if (psGlobalResourceList == nullptr)
        psGlobalResourceList = psResource;
    else
        psGlobalResourceLast->psNext = psResource;
    psGlobalResourceLast = psResource;
    if (psLock)
        CPLReleaseLock(psLock);
}

static void ThreadFuncDedicatedDataset(void *_psThreadDescription)
{
    ThreadDescription *psThreadDescription =
        (ThreadDescription *)_psThreadDescription;
    int nXSize = psThreadDescription->poDS->GetRasterXSize();
    int nYSize = psThreadDescription->poDS->GetRasterYSize();
    void *pBuffer = CPLMalloc(psThreadDescription->nBufferSize);
    while (psThreadDescription->psRequestList != nullptr)
    {
        Request *psRequest = GetNextRequest(psThreadDescription->psRequestList);
        ReadRaster(psThreadDescription->poDS, nXSize, nYSize, psRequest->nBands,
                   (GByte *)pBuffer, psRequest->nXOff, psRequest->nYOff,
                   psRequest->nXWin, psRequest->nYWin);
        CPLFree(psRequest);
    }
    CPLFree(pBuffer);
}

static void ThreadFuncWithMigration(void * /* _unused */)
{
    Request *psRequest;
    while ((psRequest = GetNextRequest(psGlobalRequestList)) != nullptr)
    {
        Resource *psResource = AcquireFirstResource();
        ASSERT_TRUE(psResource != nullptr);
        int nXSize = psResource->poDS->GetRasterXSize();
        int nYSize = psResource->poDS->GetRasterYSize();
        ReadRaster(psResource->poDS, nXSize, nYSize, psRequest->nBands,
                   (GByte *)psResource->pBuffer, psRequest->nXOff,
                   psRequest->nYOff, psRequest->nXWin, psRequest->nYWin);
        CPLFree(psRequest);
        PutResourceAtEnd(psResource);
    }
}

static int CreateRandomStrategyRequests(GDALDataset *poDS, int nMaxRequests,
                                        Request *&psRequestList,
                                        Request *&psRequestLast)
{
    unsigned long seed = 1;
    int nXSize = poDS->GetRasterXSize();
    int nYSize = poDS->GetRasterYSize();
    int nMaxXWin = MIN(1000, nXSize / 10 + 1);
    int nMaxYWin = MIN(1000, nYSize / 10 + 1);
    int nQueriedBands = MIN(4, poDS->GetRasterCount());
    int nAverageIterationsToReadWholeFile =
        ((nXSize + nMaxXWin / 2 - 1) / (nMaxXWin / 2)) *
        ((nYSize + nMaxYWin / 2 - 1) / (nMaxYWin / 2));
    int nLocalLoops = nLoops * nAverageIterationsToReadWholeFile;
    for (int iLoop = 0; iLoop < nLocalLoops; iLoop++)
    {
        if (nMaxRequests > 0 && iLoop == nMaxRequests)
            break;
        int nXOff = (int)((GIntBig)myrand_r(&seed) * (nXSize - 1) / MYRAND_MAX);
        int nYOff = (int)((GIntBig)myrand_r(&seed) * (nYSize - 1) / MYRAND_MAX);
        int nXWin = 1 + (int)((GIntBig)myrand_r(&seed) * nMaxXWin / MYRAND_MAX);
        int nYWin = 1 + (int)((GIntBig)myrand_r(&seed) * nMaxYWin / MYRAND_MAX);
        if (nXOff + nXWin > nXSize)
            nXWin = nXSize - nXOff;
        if (nYOff + nYWin > nYSize)
            nYWin = nYSize - nYOff;
        AddRequest(psRequestList, psRequestLast, nXOff, nYOff, nXWin, nYWin,
                   nQueriedBands);
    }
    return nQueriedBands * nMaxXWin * nMaxYWin;
}

static int CreateLineStrategyRequests(GDALDataset *poDS, int nMaxRequests,
                                      Request *&psRequestList,
                                      Request *&psRequestLast)
{
    int nXSize = poDS->GetRasterXSize();
    int nYSize = poDS->GetRasterYSize();
    int nQueriedBands = MIN(4, poDS->GetRasterCount());
    int bStop = FALSE;
    int nRequests = 0;
    for (int iLoop = 0; !bStop && iLoop < nLoops; iLoop++)
    {
        for (int nYOff = 0; nYOff < nYSize; nYOff++)
        {
            if (nMaxRequests > 0 && nRequests == nMaxRequests)
            {
                bStop = TRUE;
                break;
            }
            AddRequest(psRequestList, psRequestLast, 0, nYOff, nXSize, 1,
                       nQueriedBands);
            nRequests++;
        }
    }
    return nQueriedBands * nXSize;
}

static int CreateBlockStrategyRequests(GDALDataset *poDS, int nMaxRequests,
                                       Request *&psRequestList,
                                       Request *&psRequestLast)
{
    int nXSize = poDS->GetRasterXSize();
    int nYSize = poDS->GetRasterYSize();
    int nMaxXWin = MIN(1000, nXSize / 10 + 1);
    int nMaxYWin = MIN(1000, nYSize / 10 + 1);
    int nQueriedBands = MIN(4, poDS->GetRasterCount());
    int bStop = FALSE;
    int nRequests = 0;
    for (int iLoop = 0; !bStop && iLoop < nLoops; iLoop++)
    {
        for (int nYOff = 0; !bStop && nYOff < nYSize; nYOff += nMaxYWin)
        {
            int nReqYSize =
                (nYOff + nMaxYWin > nYSize) ? nYSize - nYOff : nMaxYWin;
            for (int nXOff = 0; nXOff < nXSize; nXOff += nMaxXWin)
            {
                if (nMaxRequests > 0 && nRequests == nMaxRequests)
                {
                    bStop = TRUE;
                    break;
                }
                int nReqXSize =
                    (nXOff + nMaxXWin > nXSize) ? nXSize - nXOff : nMaxXWin;
                AddRequest(psRequestList, psRequestLast, nXOff, nYOff,
                           nReqXSize, nReqYSize, nQueriedBands);
                nRequests++;
            }
        }
    }
    return nQueriedBands * nMaxXWin * nMaxYWin;
}

TEST(testblockcache, test)
{
    int i;
    int nThreads = CPLGetNumCPUs();
    std::vector<CPLJoinableThread *> apsThreads;
    Strategy eStrategy = STRATEGY_RANDOM;
    int bNewDatasetOption = FALSE;
    int nXSize = 5000;
    int nYSize = 5000;
    int nBands = 4;
    char **papszOptions = nullptr;
    int bOnDisk = FALSE;
    std::vector<ThreadDescription> asThreadDescription;
    int bMemDriver = FALSE;
    GDALDataset *poMEMDS = nullptr;
    int bMigrate = FALSE;
    int nMaxRequests = -1;

    GDALAllRegister();

    int argc = global_argc;
    char **argv = global_argv;
    for (i = 1; i < argc; i++)
    {
        if (EQUAL(argv[i], "-threads") && i + 1 < argc)
        {
            i++;
            nThreads = atoi(argv[i]);
        }
        else if (EQUAL(argv[i], "-loops") && i + 1 < argc)
        {
            i++;
            nLoops = atoi(argv[i]);
            if (nLoops <= 0)
                nLoops = INT_MAX;
        }
        else if (EQUAL(argv[i], "-max_requests") && i + 1 < argc)
        {
            i++;
            nMaxRequests = atoi(argv[i]);
        }
        else if (EQUAL(argv[i], "-strategy") && i + 1 < argc)
        {
            i++;
            if (EQUAL(argv[i], "random"))
                eStrategy = STRATEGY_RANDOM;
            else if (EQUAL(argv[i], "line"))
                eStrategy = STRATEGY_LINE;
            else if (EQUAL(argv[i], "block"))
                eStrategy = STRATEGY_BLOCK;
            else
                Usage();
        }
        else if (EQUAL(argv[i], "-xsize") && i + 1 < argc)
        {
            i++;
            nXSize = atoi(argv[i]);
            bNewDatasetOption = TRUE;
        }
        else if (EQUAL(argv[i], "-ysize") && i + 1 < argc)
        {
            i++;
            nYSize = atoi(argv[i]);
            bNewDatasetOption = TRUE;
        }
        else if (EQUAL(argv[i], "-bands") && i + 1 < argc)
        {
            i++;
            nBands = atoi(argv[i]);
            bNewDatasetOption = TRUE;
        }
        else if (EQUAL(argv[i], "-co") && i + 1 < argc)
        {
            i++;
            papszOptions = CSLAddString(papszOptions, argv[i]);
            bNewDatasetOption = TRUE;
        }
        else if (EQUAL(argv[i], "-ondisk"))
        {
            bOnDisk = TRUE;
            bNewDatasetOption = TRUE;
        }
        else if (EQUAL(argv[i], "-check"))
        {
            bCheck = TRUE;
            bNewDatasetOption = TRUE;
        }
        else if (EQUAL(argv[i], "-memdriver"))
        {
            bMemDriver = TRUE;
            bNewDatasetOption = TRUE;
        }
        else if (EQUAL(argv[i], "-migrate"))
            bMigrate = TRUE;
        else if (argv[i][0] == '-')
            Usage();
        else if (pszDataset == nullptr)
            pszDataset = argv[i];
        else
        {
            Usage();
        }
    }

    if (pszDataset != nullptr && bNewDatasetOption)
        Usage();

    CPLDebug("TEST", "Using %d threads", nThreads);

    int bCreatedDataset = FALSE;
    if (pszDataset == nullptr)
    {
        bCreatedDataset = TRUE;
        if (bOnDisk)
            pszDataset = "/tmp/tmp.tif";
        else
            pszDataset = "/vsimem/tmp.tif";
        GDALDataset *poDS =
            ((GDALDriver *)GDALGetDriverByName((bMemDriver) ? "MEM" : "GTiff"))
                ->Create(pszDataset, nXSize, nYSize, nBands, GDT_Byte,
                         papszOptions);
        if (bCheck)
        {
            GByte *pabyLine =
                (GByte *)VSIMalloc(static_cast<size_t>(nBands) * nXSize);
            for (int iY = 0; iY < nYSize; iY++)
            {
                for (int iX = 0; iX < nXSize; iX++)
                {
                    for (int iBand = 0; iBand < nBands; iBand++)
                    {
                        unsigned long seed =
                            iBand * nXSize * nYSize + iY * nXSize + iX;
                        pabyLine[iBand * nXSize + iX] =
                            (GByte)(myrand_r(&seed) & 0xff);
                    }
                }
                CPL_IGNORE_RET_VAL(poDS->RasterIO(GF_Write, 0, iY, nXSize, 1,
                                                  pabyLine, nXSize, 1, GDT_Byte,
                                                  nBands, nullptr, 0, 0, 0
#ifdef GDAL_COMPILATION
                                                  ,
                                                  nullptr
#endif
                                                  ));
            }
            VSIFree(pabyLine);
        }
        if (bMemDriver)
            poMEMDS = poDS;
        else
            GDALClose(poDS);
    }
    else
    {
        bCheck = FALSE;
    }
    CSLDestroy(papszOptions);
    papszOptions = nullptr;

    Request *psGlobalRequestLast = nullptr;

    for (i = 0; i < nThreads; i++)
    {
        GDALDataset *poDS;
        // Since GDAL 2.0, the MEM driver is thread-safe, i.e. does not use the
        // block cache, but only for operations not involving resampling, which
        // is the case here
        if (poMEMDS)
            poDS = poMEMDS;
        else
        {
            poDS = (GDALDataset *)GDALOpen(pszDataset, GA_ReadOnly);
            if (poDS == nullptr)
                exit(1);
        }
        if (bMigrate)
        {
            Resource *psResource = (Resource *)CPLMalloc(sizeof(Resource));
            psResource->poDS = poDS;
            int nBufferSize;
            if (eStrategy == STRATEGY_RANDOM)
                nBufferSize = CreateRandomStrategyRequests(poDS, nMaxRequests,
                                                           psGlobalRequestList,
                                                           psGlobalRequestLast);
            else if (eStrategy == STRATEGY_LINE)
                nBufferSize = CreateLineStrategyRequests(poDS, nMaxRequests,
                                                         psGlobalRequestList,
                                                         psGlobalRequestLast);
            else
                nBufferSize = CreateBlockStrategyRequests(poDS, nMaxRequests,
                                                          psGlobalRequestList,
                                                          psGlobalRequestLast);
            psResource->pBuffer = CPLMalloc(nBufferSize);
            PutResourceAtEnd(psResource);
        }
        else
        {
            ThreadDescription sThreadDescription;
            sThreadDescription.poDS = poDS;
            sThreadDescription.psRequestList = nullptr;
            Request *psRequestLast = nullptr;
            if (eStrategy == STRATEGY_RANDOM)
                sThreadDescription.nBufferSize = CreateRandomStrategyRequests(
                    poDS, nMaxRequests, sThreadDescription.psRequestList,
                    psRequestLast);
            else if (eStrategy == STRATEGY_LINE)
                sThreadDescription.nBufferSize = CreateLineStrategyRequests(
                    poDS, nMaxRequests, sThreadDescription.psRequestList,
                    psRequestLast);
            else
                sThreadDescription.nBufferSize = CreateBlockStrategyRequests(
                    poDS, nMaxRequests, sThreadDescription.psRequestList,
                    psRequestLast);
            asThreadDescription.push_back(sThreadDescription);
        }
    }

    if (bCreatedDataset && poMEMDS == nullptr && bOnDisk)
    {
        CPLPushErrorHandler(CPLQuietErrorHandler);
        VSIUnlink(pszDataset);
        CPLPopErrorHandler();
    }

    if (bMigrate)
    {
        psLock = CPLCreateLock(LOCK_SPIN);
    }

    for (i = 0; i < nThreads; i++)
    {
        CPLJoinableThread *pThread;
        if (bMigrate)
            pThread = CPLCreateJoinableThread(ThreadFuncWithMigration, nullptr);
        else
            pThread = CPLCreateJoinableThread(ThreadFuncDedicatedDataset,
                                              &(asThreadDescription[i]));
        apsThreads.push_back(pThread);
    }
    for (i = 0; i < nThreads; i++)
    {
        CPLJoinThread(apsThreads[i]);
        if (!bMigrate && poMEMDS == nullptr)
            GDALClose(asThreadDescription[i].poDS);
    }
    while (psGlobalResourceList != nullptr)
    {
        CPLFree(psGlobalResourceList->pBuffer);
        if (poMEMDS == nullptr)
            GDALClose(psGlobalResourceList->poDS);
        Resource *psNext = psGlobalResourceList->psNext;
        CPLFree(psGlobalResourceList);
        psGlobalResourceList = psNext;
    }

    if (psLock)
    {
        CPLDestroyLock(psLock);
    }

    if (bCreatedDataset && poMEMDS == nullptr)
    {
        CPLPushErrorHandler(CPLQuietErrorHandler);
        VSIUnlink(pszDataset);
        CPLPopErrorHandler();
    }
    if (poMEMDS)
        GDALClose(poMEMDS);

    EXPECT_EQ(GDALGetCacheUsed64(), 0);

    GDALDestroyDriverManager();
}

}  // namespace
