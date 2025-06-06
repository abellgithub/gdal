/******************************************************************************
 *
 * Project:  Contour Generation
 * Purpose:  Core algorithm implementation for contour line generation.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2003, Applied Coherent Technology Corporation, www.actgate.com
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2018, Oslandia <infos at oslandia dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "level_generator.h"
#include "polygon_ring_appender.h"
#include "utility.h"
#include "contour_generator.h"
#include "segment_merger.h"
#include <algorithm>

#include "gdal.h"
#include "gdal_alg.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogr_api.h"
#include "ogr_srs_api.h"
#include "ogr_geometry.h"

#include <climits>
#include <limits>

static CPLErr OGRPolygonContourWriter(double dfLevelMin, double dfLevelMax,
                                      const OGRMultiPolygon &multipoly,
                                      void *pInfo)

{
    OGRContourWriterInfo *poInfo = static_cast<OGRContourWriterInfo *>(pInfo);

    OGRFeatureDefnH hFDefn =
        OGR_L_GetLayerDefn(static_cast<OGRLayerH>(poInfo->hLayer));

    OGRFeatureH hFeat = OGR_F_Create(hFDefn);

    if (poInfo->nIDField != -1)
        OGR_F_SetFieldInteger(hFeat, poInfo->nIDField, poInfo->nNextID++);

    if (poInfo->nElevFieldMin != -1)
        OGR_F_SetFieldDouble(hFeat, poInfo->nElevFieldMin, dfLevelMin);

    if (poInfo->nElevFieldMax != -1)
        OGR_F_SetFieldDouble(hFeat, poInfo->nElevFieldMax, dfLevelMax);

    const bool bHasZ = wkbHasZ(OGR_FD_GetGeomType(hFDefn));
    OGRGeometryH hGeom =
        OGR_G_CreateGeometry(bHasZ ? wkbMultiPolygon25D : wkbMultiPolygon);

    for (int iPart = 0; iPart < multipoly.getNumGeometries(); iPart++)
    {
        OGRPolygon *poNewPoly = new OGRPolygon();
        const OGRPolygon *poPolygon =
            static_cast<const OGRPolygon *>(multipoly.getGeometryRef(iPart));

        for (int iRing = 0; iRing < poPolygon->getNumInteriorRings() + 1;
             iRing++)
        {
            const OGRLinearRing *poRing =
                iRing == 0 ? poPolygon->getExteriorRing()
                           : poPolygon->getInteriorRing(iRing - 1);

            OGRLinearRing *poNewRing = new OGRLinearRing();
            for (int iPoint = 0; iPoint < poRing->getNumPoints(); iPoint++)
            {
                const double dfX =
                    poInfo->adfGeoTransform[0] +
                    poInfo->adfGeoTransform[1] * poRing->getX(iPoint) +
                    poInfo->adfGeoTransform[2] * poRing->getY(iPoint);
                const double dfY =
                    poInfo->adfGeoTransform[3] +
                    poInfo->adfGeoTransform[4] * poRing->getX(iPoint) +
                    poInfo->adfGeoTransform[5] * poRing->getY(iPoint);
                if (bHasZ)
                    OGR_G_SetPoint(OGRGeometry::ToHandle(poNewRing), iPoint,
                                   dfX, dfY, dfLevelMax);
                else
                    OGR_G_SetPoint_2D(OGRGeometry::ToHandle(poNewRing), iPoint,
                                      dfX, dfY);
            }
            poNewPoly->addRingDirectly(poNewRing);
        }
        OGR_G_AddGeometryDirectly(hGeom, OGRGeometry::ToHandle(poNewPoly));
    }

    OGR_F_SetGeometryDirectly(hFeat, hGeom);

    OGRErr eErr =
        OGR_L_CreateFeature(static_cast<OGRLayerH>(poInfo->hLayer), hFeat);
    OGR_F_Destroy(hFeat);

    if (eErr == OGRERR_NONE && poInfo->nTransactionCommitInterval > 0)
    {
        if (++poInfo->nWrittenFeatureCountSinceLastCommit ==
            poInfo->nTransactionCommitInterval)
        {
            poInfo->nWrittenFeatureCountSinceLastCommit = 0;
            // CPLDebug("CONTOUR", "Flush transaction");
            eErr =
                OGR_L_CommitTransaction(static_cast<OGRLayerH>(poInfo->hLayer));
            if (eErr == OGRERR_NONE)
            {
                eErr = OGR_L_StartTransaction(
                    static_cast<OGRLayerH>(poInfo->hLayer));
            }
        }
    }

    return eErr == OGRERR_NONE ? CE_None : CE_Failure;
}

struct PolygonContourWriter
{
    CPL_DISALLOW_COPY_ASSIGN(PolygonContourWriter)

    explicit PolygonContourWriter(OGRContourWriterInfo *poInfo, double minLevel)
        : poInfo_(poInfo), currentLevel_(minLevel)
    {
    }

    void startPolygon(double level)
    {
        previousLevel_ = currentLevel_;
        currentGeometry_.reset(new OGRMultiPolygon());
        currentLevel_ = level;
    }

    void endPolygon()
    {
        if (currentPart_)
        {
            currentGeometry_->addGeometryDirectly(currentPart_);
        }

        if (currentGeometry_->getNumGeometries() > 0)
        {
            OGRPolygonContourWriter(previousLevel_, currentLevel_,
                                    *currentGeometry_, poInfo_);
        }

        currentGeometry_.reset(nullptr);
        currentPart_ = nullptr;
    }

    void addPart(const marching_squares::LineString &ring)
    {
        if (currentPart_)
        {
            currentGeometry_->addGeometryDirectly(currentPart_);
        }

        OGRLinearRing *poNewRing = new OGRLinearRing();
        poNewRing->setNumPoints(int(ring.size()));
        int iPoint = 0;
        for (const auto &p : ring)
        {
            poNewRing->setPoint(iPoint, p.x, p.y);
            iPoint++;
        }
        currentPart_ = new OGRPolygon();
        currentPart_->addRingDirectly(poNewRing);
    }

    void addInteriorRing(const marching_squares::LineString &ring)
    {
        OGRLinearRing *poNewRing = new OGRLinearRing();
        for (const auto &p : ring)
        {
            poNewRing->addPoint(p.x, p.y);
        }
        currentPart_->addRingDirectly(poNewRing);
    }

    std::unique_ptr<OGRMultiPolygon> currentGeometry_ = {};
    OGRPolygon *currentPart_ = nullptr;
    OGRContourWriterInfo *poInfo_ = nullptr;
    double currentLevel_ = 0;
    double previousLevel_ = 0;
};

struct GDALRingAppender
{
    CPL_DISALLOW_COPY_ASSIGN(GDALRingAppender)

    GDALRingAppender(GDALContourWriter write, void *data)
        : write_(write), data_(data)
    {
    }

    void addLine(double level, marching_squares::LineString &ls,
                 bool /*closed*/)
    {
        const size_t sz = ls.size();
        std::vector<double> xs(sz), ys(sz);
        size_t i = 0;
        for (const auto &pt : ls)
        {
            xs[i] = pt.x;
            ys[i] = pt.y;
            i++;
        }

        if (write_(level, int(sz), &xs[0], &ys[0], data_) != CE_None)
            CPLError(CE_Failure, CPLE_AppDefined, "cannot write linestring");
    }

  private:
    GDALContourWriter write_;
    void *data_;
};

/************************************************************************/
/* ==================================================================== */
/*                   Additional C Callable Functions                    */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                          OGRContourWriter()                          */
/************************************************************************/

CPLErr OGRContourWriter(double dfLevel, int nPoints, double *padfX,
                        double *padfY, void *pInfo)

{
    OGRContourWriterInfo *poInfo = static_cast<OGRContourWriterInfo *>(pInfo);

    OGRFeatureDefnH hFDefn =
        OGR_L_GetLayerDefn(static_cast<OGRLayerH>(poInfo->hLayer));

    OGRFeatureH hFeat = OGR_F_Create(hFDefn);

    if (poInfo->nIDField != -1)
        OGR_F_SetFieldInteger(hFeat, poInfo->nIDField, poInfo->nNextID++);

    if (poInfo->nElevField != -1)
        OGR_F_SetFieldDouble(hFeat, poInfo->nElevField, dfLevel);

    const bool bHasZ = wkbHasZ(OGR_FD_GetGeomType(hFDefn));
    OGRGeometryH hGeom =
        OGR_G_CreateGeometry(bHasZ ? wkbLineString25D : wkbLineString);

    for (int iPoint = nPoints - 1; iPoint >= 0; iPoint--)
    {
        const double dfX = poInfo->adfGeoTransform[0] +
                           poInfo->adfGeoTransform[1] * padfX[iPoint] +
                           poInfo->adfGeoTransform[2] * padfY[iPoint];
        const double dfY = poInfo->adfGeoTransform[3] +
                           poInfo->adfGeoTransform[4] * padfX[iPoint] +
                           poInfo->adfGeoTransform[5] * padfY[iPoint];
        if (bHasZ)
            OGR_G_SetPoint(hGeom, iPoint, dfX, dfY, dfLevel);
        else
            OGR_G_SetPoint_2D(hGeom, iPoint, dfX, dfY);
    }

    OGR_F_SetGeometryDirectly(hFeat, hGeom);

    const OGRErr eErr =
        OGR_L_CreateFeature(static_cast<OGRLayerH>(poInfo->hLayer), hFeat);
    OGR_F_Destroy(hFeat);

    return eErr == OGRERR_NONE ? CE_None : CE_Failure;
}

/************************************************************************/
/*                        GDALContourGenerate()                         */
/************************************************************************/

/**
 * Create vector contours from raster DEM.
 *
 * This function is kept for compatibility reason and will call the new
 * variant GDALContourGenerateEx that is more extensible and provide more
 * options.
 *
 * Details about the algorithm are also given in the documentation of the
 * new GDALContourenerateEx function.
 *
 * @param hBand The band to read raster data from.  The whole band will be
 * processed.
 *
 * @param dfContourInterval The elevation interval between contours generated.
 *
 * @param dfContourBase The "base" relative to which contour intervals are
 * applied.  This is normally zero, but could be different.  To generate 10m
 * contours at 5, 15, 25, ... the ContourBase would be 5.
 *
 * @param  nFixedLevelCount The number of fixed levels. If this is greater than
 * zero, then fixed levels will be used, and ContourInterval and ContourBase
 * are ignored.
 *
 * @param padfFixedLevels The list of fixed contour levels at which contours
 * should be generated.  It will contain FixedLevelCount entries, and may be
 * NULL if fixed levels are disabled (FixedLevelCount = 0).
 *
 * @param bUseNoData If TRUE the dfNoDataValue will be used.
 *
 * @param dfNoDataValue The value to use as a "nodata" value. That is, a
 * pixel value which should be ignored in generating contours as if the value
 * of the pixel were not known.
 *
 * @param hLayer The layer to which new contour vectors will be written.
 * Each contour will have a LINESTRING geometry attached to it.   This
 * is really of type OGRLayerH, but void * is used to avoid pulling the
 * ogr_api.h file in here.
 *
 * @param iIDField If not -1 this will be used as a field index to indicate
 * where a unique id should be written for each feature (contour) written.
 *
 * @param iElevField If not -1 this will be used as a field index to indicate
 * where the elevation value of the contour should be written.
 *
 * @param pfnProgress A GDALProgressFunc that may be used to report progress
 * to the user, or to interrupt the algorithm.  May be NULL if not required.
 *
 * @param pProgressArg The callback data for the pfnProgress function.
 *
 * @return CE_None on success or CE_Failure if an error occurs.
 */

CPLErr GDALContourGenerate(GDALRasterBandH hBand, double dfContourInterval,
                           double dfContourBase, int nFixedLevelCount,
                           double *padfFixedLevels, int bUseNoData,
                           double dfNoDataValue, void *hLayer, int iIDField,
                           int iElevField, GDALProgressFunc pfnProgress,
                           void *pProgressArg)
{
    char **options = nullptr;
    if (nFixedLevelCount > 0)
    {
        std::string values = "FIXED_LEVELS=";
        for (int i = 0; i < nFixedLevelCount; i++)
        {
            const int sz = 32;
            char *newValue = new char[sz + 1];
            if (i == nFixedLevelCount - 1)
            {
                CPLsnprintf(newValue, sz + 1, "%f", padfFixedLevels[i]);
            }
            else
            {
                CPLsnprintf(newValue, sz + 1, "%f,", padfFixedLevels[i]);
            }
            values = values + std::string(newValue);
            delete[] newValue;
        }
        options = CSLAddString(options, values.c_str());
    }
    else if (dfContourInterval != 0.0)
    {
        options =
            CSLAppendPrintf(options, "LEVEL_INTERVAL=%f", dfContourInterval);
    }

    if (dfContourBase != 0.0)
    {
        options = CSLAppendPrintf(options, "LEVEL_BASE=%f", dfContourBase);
    }

    if (bUseNoData)
    {
        options = CSLAppendPrintf(options, "NODATA=%.19g", dfNoDataValue);
    }
    if (iIDField != -1)
    {
        options = CSLAppendPrintf(options, "ID_FIELD=%d", iIDField);
    }
    if (iElevField != -1)
    {
        options = CSLAppendPrintf(options, "ELEV_FIELD=%d", iElevField);
    }

    CPLErr err = GDALContourGenerateEx(hBand, hLayer, options, pfnProgress,
                                       pProgressArg);
    CSLDestroy(options);

    return err;
}

/**
 * Create vector contours from raster DEM.
 *
 * This algorithm is an implementation of "Marching squares" [1] that will
 * generate contour vectors for the input raster band on the requested set
 * of contour levels.  The vector contours are written to the passed in OGR
 * vector layer. Also, a NODATA value may be specified to identify pixels
 * that should not be considered in contour line generation.
 *
 * The gdal/apps/gdal_contour_bin.cpp mainline can be used as an example of
 * how to use this function.
 *
 * [1] see https://en.wikipedia.org/wiki/Marching_squares
 *
 * ALGORITHM RULES

For contouring purposes raster pixel values are assumed to represent a point
value at the center of the corresponding pixel region.  For the purpose of
contour generation we virtually connect each pixel center to the values to
the left, right, top and bottom.  We assume that the pixel value is linearly
interpolated between the pixel centers along each line, and determine where
(if any) contour lines will appear along these line segments.  Then the
contour crossings are connected.

This means that contour lines' nodes will not actually be on pixel edges, but
rather along vertical and horizontal lines connecting the pixel centers.

\verbatim
General Case:

      5 |                  | 3
     -- + ---------------- + --
        |                  |
        |                  |
        |                  |
        |                  |
     10 +                  |
        |\                 |
        | \                |
     -- + -+-------------- + --
     12 |  10              | 1

Saddle Point:

      5 |                  | 12
     -- + -------------+-- + --
        |               \  |
        |                 \|
        |                  +
        |                  |
        +                  |
        |\                 |
        | \                |
     -- + -+-------------- + --
     12 |                  | 1

or:

      5 |                  | 12
     -- + -------------+-- + --
        |          __/     |
        |      ___/        |
        |  ___/          __+
        | /           __/  |
        +'         __/     |
        |       __/        |
        |   ,__/           |
     -- + -+-------------- + --
     12 |                  | 1
\endverbatim

Nodata:

In the "nodata" case we treat the whole nodata pixel as a no-mans land.
We extend the corner pixels near the nodata out to half way and then
construct extra lines from those points to the center which is assigned
an averaged value from the two nearby points (in this case (12+3+5)/3).

\verbatim
      5 |                  | 3
     -- + ---------------- + --
        |                  |
        |                  |
        |      6.7         |
        |        +---------+ 3
     10 +___     |
        |   \____+ 10
        |        |
     -- + -------+        +
     12 |       12           (nodata)

\endverbatim

 *
 * @param hBand The band to read raster data from.  The whole band will be
 * processed.
 *
 * @param hLayer The layer to which new contour vectors will be written.
 * Each contour will have a LINESTRING geometry attached to it
 * (or POLYGON if POLYGONIZE=YES). This is really of type OGRLayerH, but
 * void * is used to avoid pulling the ogr_api.h file in here.
 *
 * @param pfnProgress A GDALProgressFunc that may be used to report progress
 * to the user, or to interrupt the algorithm.  May be NULL if not required.
 *
 * @param pProgressArg The callback data for the pfnProgress function.
 *
 * @param options List of options
 *
 * Options:
 *
 *   LEVEL_INTERVAL=f
 *
 * The elevation interval between contours generated.
 *
 *   LEVEL_BASE=f
 *
 * The "base" relative to which contour intervals are
 * applied.  This is normally zero, but could be different.  To generate 10m
 * contours at 5, 15, 25, ... the LEVEL_BASE would be 5.
 *
 *   LEVEL_EXP_BASE=f
 *
 * If greater than 0, contour levels are generated on an
 * exponential scale. Levels will then be generated by LEVEL_EXP_BASE^k
 * where k is a positive integer.
 *
 *   FIXED_LEVELS=f[,f]*
 *
 * The list of fixed contour levels at which contours should be generated.
 * This option has precedence on LEVEL_INTERVAL. MIN and MAX can be used
 * as special values to represent the minimum and maximum values of the
 * raster.
 *
 *   NODATA=f
 *
 * The value to use as a "nodata" value. That is, a pixel value which
 * should be ignored in generating contours as if the value of the pixel
 * were not known.
 *
 *   ID_FIELD=d
 *
 * This will be used as a field index to indicate where a unique id should
 * be written for each feature (contour) written.
 *
 *   ELEV_FIELD=d
 *
 * This will be used as a field index to indicate where the elevation value
 * of the contour should be written. Only used in line contouring mode.
 *
 *   ELEV_FIELD_MIN=d
 *
 * This will be used as a field index to indicate where the minimum elevation
value
 * of the polygon contour should be written. Only used in polygonal contouring
mode.
 *
 *   ELEV_FIELD_MAX=d
 *
 * This will be used as a field index to indicate where the maximum elevation
value
 * of the polygon contour should be written. Only used in polygonal contouring
mode.
 *
 *   POLYGONIZE=YES|NO
 *
 * If YES, contour polygons will be created, rather than polygon lines.
 *
 *
 *   COMMIT_INTERVAL=num
 *
 * (GDAL >= 3.10) Interval in number of features at which transactions must be
 * flushed. The default value of 0 means that no transactions are opened.
 * A negative value means a single transaction. The function takes care of
 * issuing the starting transaction and committing the final one.
 *
 * @return CE_None on success or CE_Failure if an error occurs.
 */
CPLErr GDALContourGenerateEx(GDALRasterBandH hBand, void *hLayer,
                             CSLConstList options, GDALProgressFunc pfnProgress,
                             void *pProgressArg)
{
    VALIDATE_POINTER1(hBand, "GDALContourGenerateEx", CE_Failure);

    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    double contourInterval = 0.0;
    const char *opt = CSLFetchNameValue(options, "LEVEL_INTERVAL");
    if (opt)
    {
        contourInterval = CPLAtof(opt);
        // Written this way to catch NaN as well.
        if (!(contourInterval > 0))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid value for LEVEL_INTERVAL. Should be strictly "
                     "positive.");
            return CE_Failure;
        }
    }

    double contourBase = 0.0;
    opt = CSLFetchNameValue(options, "LEVEL_BASE");
    if (opt)
    {
        contourBase = CPLAtof(opt);
    }

    double expBase = 0.0;
    opt = CSLFetchNameValue(options, "LEVEL_EXP_BASE");
    if (opt)
    {
        expBase = CPLAtof(opt);
    }

    std::vector<double> fixedLevels;
    opt = CSLFetchNameValue(options, "FIXED_LEVELS");
    if (opt)
    {
        const CPLStringList aosLevels(
            CSLTokenizeStringComplex(opt, ",", FALSE, FALSE));
        fixedLevels.resize(aosLevels.size());
        for (size_t i = 0; i < fixedLevels.size(); i++)
        {
            // Handle min/max values
            if (EQUAL(aosLevels[i], "MIN"))
            {
                fixedLevels[i] = std::numeric_limits<double>::lowest();
            }
            else if (EQUAL(aosLevels[i], "MAX"))
            {
                fixedLevels[i] = std::numeric_limits<double>::max();
            }
            else
            {
                fixedLevels[i] = CPLAtof(aosLevels[i]);
            }
            if (i > 0 && !(fixedLevels[i] >= fixedLevels[i - 1]))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "FIXED_LEVELS should be strictly increasing");
                return CE_Failure;
            }
        }
    }

    bool useNoData = false;
    double noDataValue = 0.0;
    opt = CSLFetchNameValue(options, "NODATA");
    if (opt)
    {
        useNoData = true;
        noDataValue = CPLAtof(opt);
        if (GDALGetRasterDataType(hBand) == GDT_Float32)
        {
            int bClamped = FALSE;
            int bRounded = FALSE;
            noDataValue = GDALAdjustValueToDataType(GDT_Float32, noDataValue,
                                                    &bClamped, &bRounded);
        }
    }

    int idField = -1;
    opt = CSLFetchNameValue(options, "ID_FIELD");
    if (opt)
    {
        idField = atoi(opt);
    }

    int elevField = -1;
    opt = CSLFetchNameValue(options, "ELEV_FIELD");
    if (opt)
    {
        elevField = atoi(opt);
    }

    int elevFieldMin = -1;
    opt = CSLFetchNameValue(options, "ELEV_FIELD_MIN");
    if (opt)
    {
        elevFieldMin = atoi(opt);
    }

    int elevFieldMax = -1;
    opt = CSLFetchNameValue(options, "ELEV_FIELD_MAX");
    if (opt)
    {
        elevFieldMax = atoi(opt);
    }

    bool polygonize = CPLFetchBool(options, "POLYGONIZE", false);

    using namespace marching_squares;

    OGRContourWriterInfo oCWI;
    oCWI.hLayer = static_cast<OGRLayerH>(hLayer);
    oCWI.nElevField = elevField;
    oCWI.nElevFieldMin = elevFieldMin;
    oCWI.nElevFieldMax = elevFieldMax;
    oCWI.nIDField = idField;
    oCWI.adfGeoTransform[0] = 0.0;
    oCWI.adfGeoTransform[1] = 1.0;
    oCWI.adfGeoTransform[2] = 0.0;
    oCWI.adfGeoTransform[3] = 0.0;
    oCWI.adfGeoTransform[4] = 0.0;
    oCWI.adfGeoTransform[5] = 1.0;
    GDALDatasetH hSrcDS = GDALGetBandDataset(hBand);
    if (hSrcDS != nullptr)
        GDALGetGeoTransform(hSrcDS, oCWI.adfGeoTransform);
    oCWI.nNextID = 0;
    oCWI.nWrittenFeatureCountSinceLastCommit = 0;
    oCWI.nTransactionCommitInterval =
        CPLAtoGIntBig(CSLFetchNameValueDef(options, "COMMIT_INTERVAL", "0"));

    if (oCWI.nTransactionCommitInterval < 0)
        oCWI.nTransactionCommitInterval = std::numeric_limits<GIntBig>::max();
    if (oCWI.nTransactionCommitInterval > 0)
    {
        if (OGR_L_StartTransaction(static_cast<OGRLayerH>(hLayer)) !=
            OGRERR_NONE)
        {
            oCWI.nTransactionCommitInterval = 0;
        }
    }

    int bSuccessMin = FALSE;
    double dfMinimum = GDALGetRasterMinimum(hBand, &bSuccessMin);
    int bSuccessMax = FALSE;
    double dfMaximum = GDALGetRasterMaximum(hBand, &bSuccessMax);
    if ((!bSuccessMin || !bSuccessMax))
    {
        double adfMinMax[2];
        if (GDALComputeRasterMinMax(hBand, false, adfMinMax) == CE_None)
        {
            dfMinimum = adfMinMax[0];
            dfMaximum = adfMinMax[1];
        }
    }

    bool ok = true;

    // Replace fixed levels min/max values with raster min/max values
    if (!fixedLevels.empty())
    {
        if (fixedLevels[0] == std::numeric_limits<double>::lowest())
        {
            fixedLevels[0] = dfMinimum;
        }
        if (fixedLevels.back() == std::numeric_limits<double>::max())
        {
            fixedLevels.back() = dfMaximum;
        }
    }

    try
    {
        if (polygonize)
        {

            if (!fixedLevels.empty())
            {

                // If the minimum raster value is larger than the first requested
                // level, select the requested level that is just below the
                // minimum raster value
                if (fixedLevels[0] < dfMinimum)
                {
                    for (size_t i = 1; i < fixedLevels.size(); ++i)
                    {
                        if (fixedLevels[i] >= dfMinimum)
                        {
                            dfMinimum = fixedLevels[i - 1];
                            break;
                        }
                    }
                }

                // Largest requested level (levels are sorted)
                const double maxLevel{fixedLevels.back()};

                // If the maximum raster value is smaller than the last requested
                // level, select the requested level that is just above the
                // maximum raster value
                if (maxLevel > dfMaximum)
                {
                    for (size_t i = fixedLevels.size() - 1; i > 0; --i)
                    {
                        if (fixedLevels[i] <= dfMaximum)
                        {
                            dfMaximum = fixedLevels[i + 1];
                            break;
                        }
                    }
                }

                // If the maximum raster value is equal to the last requested
                // level, add a small value to the maximum to avoid skipping the
                // last level polygons
                if (maxLevel == dfMaximum)
                {
                    dfMaximum = std::nextafter(
                        dfMaximum, std::numeric_limits<double>::infinity());
                }
            }

            PolygonContourWriter w(&oCWI, dfMinimum);
            typedef PolygonRingAppender<PolygonContourWriter> RingAppender;
            RingAppender appender(w);

            if (expBase > 0.0)
            {
                // Do not provide the actual minimum value to level iterator
                // in polygonal case, otherwise it can result in a polygon
                // with a degenerate min=max range.
                ExponentialLevelRangeIterator generator(
                    expBase, -std::numeric_limits<double>::infinity());
                auto levelIt{generator.range(dfMinimum, dfMaximum)};
                for (auto i = levelIt.begin(); i != levelIt.end(); ++i)
                {
                    const double level = (*i).second;
                    fixedLevels.push_back(level);
                }
                // Append minimum value to fixed levels
                fixedLevels.push_back(dfMinimum);
                // Append maximum value to fixed levels
                fixedLevels.push_back(dfMaximum);
            }
            else if (contourInterval != 0)
            {
                // Do not provide the actual minimum value to level iterator
                // in polygonal case, otherwise it can result in a polygon
                // with a degenerate min=max range.
                IntervalLevelRangeIterator generator(
                    contourBase, contourInterval,
                    -std::numeric_limits<double>::infinity());
                auto levelIt{generator.range(dfMinimum, dfMaximum)};
                for (auto i = levelIt.begin(); i != levelIt.end(); ++i)
                {
                    const double level = (*i).second;
                    fixedLevels.push_back(level);
                }
                // Append minimum value to fixed levels
                fixedLevels.push_back(dfMinimum);
                // Append maximum value to fixed levels
                fixedLevels.push_back(dfMaximum);
            }

            if (!fixedLevels.empty())
            {
                std::sort(fixedLevels.begin(), fixedLevels.end());
                auto uniqueIt =
                    std::unique(fixedLevels.begin(), fixedLevels.end());
                fixedLevels.erase(uniqueIt, fixedLevels.end());
                // Do not provide the actual minimum value to level iterator
                // in polygonal case, otherwise it can result in a polygon
                // with a degenerate min=max range.
                FixedLevelRangeIterator levels(
                    &fixedLevels[0], fixedLevels.size(),
                    -std::numeric_limits<double>::infinity(), dfMaximum);
                SegmentMerger<RingAppender, FixedLevelRangeIterator> writer(
                    appender, levels, /* polygonize */ true);
                std::vector<int> aoiSkipLevels;
                // Skip first and last levels (min/max) in polygonal case
                aoiSkipLevels.push_back(0);
                aoiSkipLevels.push_back(static_cast<int>(levels.levelsCount()));
                writer.setSkipLevels(aoiSkipLevels);
                ContourGeneratorFromRaster<decltype(writer),
                                           FixedLevelRangeIterator>
                    cg(hBand, useNoData, noDataValue, writer, levels);
                ok = cg.process(pfnProgress, pProgressArg);
            }
        }
        else
        {
            GDALRingAppender appender(OGRContourWriter, &oCWI);

            // Append all exp levels to fixed levels
            if (expBase > 0.0)
            {
                ExponentialLevelRangeIterator generator(expBase, dfMinimum);
                auto levelIt{generator.range(dfMinimum, dfMaximum)};
                for (auto i = levelIt.begin(); i != levelIt.end(); ++i)
                {
                    const double level = (*i).second;
                    fixedLevels.push_back(level);
                }
            }
            else if (contourInterval != 0)
            {
                IntervalLevelRangeIterator levels(contourBase, contourInterval,
                                                  dfMinimum);
                auto levelIt{levels.range(dfMinimum, dfMaximum)};
                for (auto i = levelIt.begin(); i != levelIt.end(); ++i)
                {
                    const double level = (*i).second;
                    fixedLevels.push_back(level);
                }
            }

            if (!fixedLevels.empty())
            {
                std::sort(fixedLevels.begin(), fixedLevels.end());
                auto uniqueIt =
                    std::unique(fixedLevels.begin(), fixedLevels.end());
                fixedLevels.erase(uniqueIt, fixedLevels.end());
                FixedLevelRangeIterator levels(
                    &fixedLevels[0], fixedLevels.size(), dfMinimum, dfMaximum);
                SegmentMerger<GDALRingAppender, FixedLevelRangeIterator> writer(
                    appender, levels, /* polygonize */ false);
                ContourGeneratorFromRaster<decltype(writer),
                                           FixedLevelRangeIterator>
                    cg(hBand, useNoData, noDataValue, writer, levels);
                ok = cg.process(pfnProgress, pProgressArg);
            }
        }
    }
    catch (const std::exception &e)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s", e.what());
        return CE_Failure;
    }

    if (oCWI.nTransactionCommitInterval > 0)
    {
        // CPLDebug("CONTOUR", "Flush transaction");
        if (OGR_L_CommitTransaction(static_cast<OGRLayerH>(hLayer)) !=
            OGRERR_NONE)
        {
            ok = false;
        }
    }

    if (ok)
    {
        pfnProgress(1.0, "", pProgressArg);
    }

    return ok ? CE_None : CE_Failure;
}

/************************************************************************/
/*                           GDAL_CG_Create()                           */
/************************************************************************/

namespace marching_squares
{

// Opaque type used by the C API
struct ContourGeneratorOpaque
{
    typedef SegmentMerger<GDALRingAppender, IntervalLevelRangeIterator>
        SegmentMergerT;
    typedef ContourGenerator<SegmentMergerT, IntervalLevelRangeIterator>
        ContourGeneratorT;

    ContourGeneratorOpaque(int nWidth, int nHeight, int bNoDataSet,
                           double dfNoDataValue, double dfContourInterval,
                           double dfContourBase, GDALContourWriter pfnWriter,
                           void *pCBData)
        : levels(dfContourBase, dfContourInterval,
                 -std::numeric_limits<double>::infinity()),
          writer(pfnWriter, pCBData),
          merger(writer, levels, /* polygonize */ false),
          contourGenerator(nWidth, nHeight, bNoDataSet != 0, dfNoDataValue,
                           merger, levels)
    {
    }

    IntervalLevelRangeIterator levels;
    GDALRingAppender writer;
    SegmentMergerT merger;
    ContourGeneratorT contourGenerator;
};

}  // namespace marching_squares

/** Create contour generator */
GDALContourGeneratorH GDAL_CG_Create(int nWidth, int nHeight, int bNoDataSet,
                                     double dfNoDataValue,
                                     double dfContourInterval,
                                     double dfContourBase,
                                     GDALContourWriter pfnWriter, void *pCBData)

{
    auto cg = new marching_squares::ContourGeneratorOpaque(
        nWidth, nHeight, bNoDataSet, dfNoDataValue, dfContourInterval,
        dfContourBase, pfnWriter, pCBData);

    return reinterpret_cast<GDALContourGeneratorH>(cg);
}

/************************************************************************/
/*                          GDAL_CG_FeedLine()                          */
/************************************************************************/

/** Feed a line to the contour generator */
CPLErr GDAL_CG_FeedLine(GDALContourGeneratorH hCG, double *padfScanline)

{
    VALIDATE_POINTER1(hCG, "GDAL_CG_FeedLine", CE_Failure);
    return reinterpret_cast<marching_squares::ContourGeneratorOpaque *>(hCG)
        ->contourGenerator.feedLine(padfScanline);
}

/************************************************************************/
/*                          GDAL_CG_Destroy()                           */
/************************************************************************/

/** Destroy contour generator */
void GDAL_CG_Destroy(GDALContourGeneratorH hCG)

{
    delete reinterpret_cast<marching_squares::ContourGeneratorOpaque *>(hCG);
}
