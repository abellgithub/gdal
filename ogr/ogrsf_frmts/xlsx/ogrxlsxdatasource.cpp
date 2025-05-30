/******************************************************************************
 *
 * Project:  XLSX Translator
 * Purpose:  Implements OGRXLSXDataSource class
 * Author:   Even Rouault, even dot rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2012-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_xlsx.h"
#include "ogr_p.h"
#include "cpl_conv.h"
#include "cpl_time.h"
#include "cpl_vsi_error.h"

#include <algorithm>

namespace OGRXLSX
{

constexpr int PARSER_BUF_SIZE = 8192;

constexpr int NUMBER_OF_DAYS_BETWEEN_1900_AND_1970 = 25569;
constexpr int NUMBER_OF_SECONDS_PER_DAY = 86400;

/************************************************************************/
/*                            OGRXLSXLayer()                            */
/************************************************************************/

OGRXLSXLayer::OGRXLSXLayer(OGRXLSXDataSource *poDSIn, const char *pszFilename,
                           const char *pszName, int bUpdatedIn)
    : OGRMemLayer(pszName, nullptr, wkbNone), bInit(CPL_TO_BOOL(bUpdatedIn)),
      poDS(poDSIn), osFilename(pszFilename), bUpdated(CPL_TO_BOOL(bUpdatedIn)),
      bHasHeaderLine(false)
{
    SetAdvertizeUTF8(true);
}

/************************************************************************/
/*                              Init()                                  */
/************************************************************************/

void OGRXLSXLayer::Init()
{
    if (!bInit)
    {
        bInit = true;
        CPLDebug("XLSX", "Init(%s)", GetName());
        poDS->BuildLayer(this);
    }
}

/************************************************************************/
/*                             Updated()                                */
/************************************************************************/

void OGRXLSXLayer::SetUpdated(bool bUpdatedIn)
{
    if (bUpdatedIn && !bUpdated && poDS->GetUpdatable())
    {
        bUpdated = true;
        poDS->SetUpdated();
    }
    else if (bUpdated && !bUpdatedIn)
    {
        bUpdated = false;
    }
}

/************************************************************************/
/*                           SyncToDisk()                               */
/************************************************************************/

OGRErr OGRXLSXLayer::SyncToDisk()
{
    poDS->FlushCache(false);
    return OGRERR_NONE;
}

/************************************************************************/
/*                      TranslateFIDFromMemLayer()                      */
/************************************************************************/

// Translate a FID from MEM convention (0-based) to XLSX convention
GIntBig OGRXLSXLayer::TranslateFIDFromMemLayer(GIntBig nFID) const
{
    return nFID + (1 + (bHasHeaderLine ? 1 : 0));
}

/************************************************************************/
/*                        TranslateFIDToMemLayer()                      */
/************************************************************************/

// Translate a FID from XLSX convention to MEM convention (0-based)
GIntBig OGRXLSXLayer::TranslateFIDToMemLayer(GIntBig nFID) const
{
    if (nFID > 0)
        return nFID - (1 + (bHasHeaderLine ? 1 : 0));
    return OGRNullFID;
}

/************************************************************************/
/*                          GetNextFeature()                            */
/************************************************************************/

OGRFeature *OGRXLSXLayer::GetNextFeature()
{
    Init();

    OGRFeature *poFeature = OGRMemLayer::GetNextFeature();
    if (poFeature)
        poFeature->SetFID(TranslateFIDFromMemLayer(poFeature->GetFID()));
    return poFeature;
}

/************************************************************************/
/*                           CreateField()                              */
/************************************************************************/

OGRErr OGRXLSXLayer::CreateField(const OGRFieldDefn *poField, int bApproxOK)
{
    Init();

    if (GetLayerDefn()->GetFieldCount() >= 2000)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Maximum number of fields supported is 2000");
        return OGRERR_FAILURE;
    }
    SetUpdated();
    return OGRMemLayer::CreateField(poField, bApproxOK);
}

/************************************************************************/
/*                           GetFeature()                               */
/************************************************************************/

OGRFeature *OGRXLSXLayer::GetFeature(GIntBig nFeatureId)
{
    Init();

    OGRFeature *poFeature =
        OGRMemLayer::GetFeature(TranslateFIDToMemLayer(nFeatureId));
    if (poFeature)
        poFeature->SetFID(nFeatureId);
    return poFeature;
}

/************************************************************************/
/*                           ISetFeature()                              */
/************************************************************************/

OGRErr OGRXLSXLayer::ISetFeature(OGRFeature *poFeature)
{
    Init();

    const GIntBig nFIDOrigin = poFeature->GetFID();
    if (nFIDOrigin > 0)
    {
        const GIntBig nFIDMemLayer = TranslateFIDToMemLayer(nFIDOrigin);
        if (!GetFeatureRef(nFIDMemLayer))
            return OGRERR_NON_EXISTING_FEATURE;
        poFeature->SetFID(nFIDMemLayer);
    }
    else
    {
        return OGRERR_NON_EXISTING_FEATURE;
    }
    SetUpdated();
    OGRErr eErr = OGRMemLayer::ISetFeature(poFeature);
    poFeature->SetFID(nFIDOrigin);
    return eErr;
}

/************************************************************************/
/*                         IUpdateFeature()                             */
/************************************************************************/

OGRErr OGRXLSXLayer::IUpdateFeature(OGRFeature *poFeature,
                                    int nUpdatedFieldsCount,
                                    const int *panUpdatedFieldsIdx,
                                    int nUpdatedGeomFieldsCount,
                                    const int *panUpdatedGeomFieldsIdx,
                                    bool bUpdateStyleString)
{
    Init();

    const GIntBig nFIDOrigin = poFeature->GetFID();
    if (nFIDOrigin != OGRNullFID)
        poFeature->SetFID(TranslateFIDToMemLayer(nFIDOrigin));
    SetUpdated();
    OGRErr eErr = OGRMemLayer::IUpdateFeature(
        poFeature, nUpdatedFieldsCount, panUpdatedFieldsIdx,
        nUpdatedGeomFieldsCount, panUpdatedGeomFieldsIdx, bUpdateStyleString);
    poFeature->SetFID(nFIDOrigin);
    return eErr;
}

/************************************************************************/
/*                          ICreateFeature()                            */
/************************************************************************/

OGRErr OGRXLSXLayer::ICreateFeature(OGRFeature *poFeature)
{
    Init();

    const GIntBig nFIDOrigin = poFeature->GetFID();
    if (nFIDOrigin > 0)
    {
        const GIntBig nFIDModified = TranslateFIDToMemLayer(nFIDOrigin);
        if (GetFeatureRef(nFIDModified))
        {
            SetUpdated();
            poFeature->SetFID(nFIDModified);
            OGRErr eErr = OGRMemLayer::ISetFeature(poFeature);
            poFeature->SetFID(nFIDOrigin);
            return eErr;
        }
    }
    SetUpdated();
    poFeature->SetFID(OGRNullFID);
    OGRErr eErr = OGRMemLayer::ICreateFeature(poFeature);
    poFeature->SetFID(TranslateFIDFromMemLayer(poFeature->GetFID()));
    return eErr;
}

/************************************************************************/
/*                          DeleteFeature()                             */
/************************************************************************/

OGRErr OGRXLSXLayer::DeleteFeature(GIntBig nFID)
{
    Init();
    SetUpdated();
    return OGRMemLayer::DeleteFeature(TranslateFIDToMemLayer(nFID));
}

/************************************************************************/
/*                             GetDataset()                             */
/************************************************************************/

GDALDataset *OGRXLSXLayer::GetDataset()
{
    return poDS;
}

/************************************************************************/
/*                          OGRXLSXDataSource()                         */
/************************************************************************/

OGRXLSXDataSource::OGRXLSXDataSource(CSLConstList papszOpenOptionsIn)
    : pszName(nullptr), bUpdatable(false), bUpdated(false), nLayers(0),
      papoLayers(nullptr), bFirstLineIsHeaders(false),
      bAutodetectTypes(!EQUAL(
          CSLFetchNameValueDef(papszOpenOptionsIn, "FIELD_TYPES",
                               CPLGetConfigOption("OGR_XLSX_FIELD_TYPES", "")),
          "STRING")),
      oParser(nullptr), bStopParsing(false), nWithoutEventCounter(0),
      nDataHandlerCounter(0), nCurLine(0), nCurCol(0), poCurLayer(nullptr),
      nStackDepth(0), nDepth(0), bInCellXFS(false)
{
    stateStack[0].eVal = STATE_DEFAULT;
    stateStack[0].nBeginDepth = 0;
}

/************************************************************************/
/*                         ~OGRXLSXDataSource()                          */
/************************************************************************/

OGRXLSXDataSource::~OGRXLSXDataSource()

{
    OGRXLSXDataSource::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr OGRXLSXDataSource::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (OGRXLSXDataSource::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        CPLFree(pszName);

        for (int i = 0; i < nLayers; i++)
            delete papoLayers[i];
        CPLFree(papoLayers);

        if (GDALDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRXLSXDataSource::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, ODsCCreateLayer))
        return bUpdatable;
    else if (EQUAL(pszCap, ODsCDeleteLayer))
        return bUpdatable;
    else if (EQUAL(pszCap, ODsCRandomLayerWrite))
        return bUpdatable;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return true;
    else if (EQUAL(pszCap, ODsCMeasuredGeometries))
        return true;
    else if (EQUAL(pszCap, ODsCCurveGeometries))
        return true;
    else
        return false;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRXLSXDataSource::GetLayer(int iLayer)

{
    if (iLayer < 0 || iLayer >= nLayers)
        return nullptr;

    return papoLayers[iLayer];
}

/************************************************************************/
/*                            GetLayerCount()                           */
/************************************************************************/

int OGRXLSXDataSource::GetLayerCount()
{
    return nLayers;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRXLSXDataSource::Open(const char *pszFilename,
                            const char *pszPrefixedFilename,
                            VSILFILE *fpWorkbook, VSILFILE *fpWorkbookRels,
                            VSILFILE *fpSharedStrings, VSILFILE *fpStyles,
                            int bUpdateIn)

{
    SetDescription(pszFilename);

    bUpdatable = CPL_TO_BOOL(bUpdateIn);

    pszName = CPLStrdup(pszFilename);
    osPrefixedFilename = pszPrefixedFilename;

    AnalyseWorkbookRels(fpWorkbookRels);
    AnalyseWorkbook(fpWorkbook);
    AnalyseSharedStrings(fpSharedStrings);
    AnalyseStyles(fpStyles);

    /* Remove empty layers at the end, which tend to be there */
    while (nLayers > 1)
    {
        papoLayers[nLayers - 1]->Init();
        if ((papoLayers[nLayers - 1]->m_osCols.empty() ||
             papoLayers[nLayers - 1]->m_osCols.find("max=\"1025\" min=\"1\"") !=
                 std::string::npos) &&
            papoLayers[nLayers - 1]->GetFeatureCount(false) == 0)
        {
            delete papoLayers[nLayers - 1];
            nLayers--;
        }
        else
            break;
    }

    return TRUE;
}

/************************************************************************/
/*                             Create()                                 */
/************************************************************************/

int OGRXLSXDataSource::Create(const char *pszFilename,
                              CPL_UNUSED char **papszOptions)
{
    bUpdated = true;
    bUpdatable = true;

    pszName = CPLStrdup(pszFilename);

    return TRUE;
}

/************************************************************************/
/*                           GetUnprefixed()                            */
/************************************************************************/

static const char *GetUnprefixed(const char *pszStr)
{
    const char *pszColumn = strchr(pszStr, ':');
    if (pszColumn)
        return pszColumn + 1;
    return pszStr;
}

/************************************************************************/
/*                           startElementCbk()                          */
/************************************************************************/

static void XMLCALL startElementCbk(void *pUserData, const char *pszNameIn,
                                    const char **ppszAttr)
{
    ((OGRXLSXDataSource *)pUserData)->startElementCbk(pszNameIn, ppszAttr);
}

void OGRXLSXDataSource::startElementCbk(const char *pszNameIn,
                                        const char **ppszAttr)
{
    if (bStopParsing)
        return;

    pszNameIn = GetUnprefixed(pszNameIn);

    nWithoutEventCounter = 0;
    switch (stateStack[nStackDepth].eVal)
    {
        case STATE_DEFAULT:
            startElementDefault(pszNameIn, ppszAttr);
            break;
        case STATE_COLS:
            startElementCols(pszNameIn, ppszAttr);
            break;
        case STATE_SHEETDATA:
            startElementTable(pszNameIn, ppszAttr);
            break;
        case STATE_ROW:
            startElementRow(pszNameIn, ppszAttr);
            break;
        case STATE_CELL:
            startElementCell(pszNameIn, ppszAttr);
            break;
        case STATE_TEXTV:
            break;
        default:
            break;
    }
    nDepth++;
}

/************************************************************************/
/*                            endElementCbk()                           */
/************************************************************************/

static void XMLCALL endElementCbk(void *pUserData, const char *pszNameIn)
{
    ((OGRXLSXDataSource *)pUserData)->endElementCbk(pszNameIn);
}

void OGRXLSXDataSource::endElementCbk(const char *pszNameIn)
{
    if (bStopParsing)
        return;

    pszNameIn = GetUnprefixed(pszNameIn);

    nWithoutEventCounter = 0;

    nDepth--;
    switch (stateStack[nStackDepth].eVal)
    {
        case STATE_DEFAULT:
            break;
        case STATE_SHEETDATA:
            endElementTable(pszNameIn);
            break;
        case STATE_COLS:
            endElementCols(pszNameIn);
            break;
        case STATE_ROW:
            endElementRow(pszNameIn);
            break;
        case STATE_CELL:
            endElementCell(pszNameIn);
            break;
        case STATE_TEXTV:
            break;
        default:
            break;
    }

    if (stateStack[nStackDepth].nBeginDepth == nDepth)
        nStackDepth--;
}

/************************************************************************/
/*                            dataHandlerCbk()                          */
/************************************************************************/

static void XMLCALL dataHandlerCbk(void *pUserData, const char *data, int nLen)
{
    ((OGRXLSXDataSource *)pUserData)->dataHandlerCbk(data, nLen);
}

void OGRXLSXDataSource::dataHandlerCbk(const char *data, int nLen)
{
    if (bStopParsing)
        return;

    nDataHandlerCounter++;
    if (nDataHandlerCounter >= PARSER_BUF_SIZE)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "File probably corrupted (million laugh pattern)");
        XML_StopParser(oParser, XML_FALSE);
        bStopParsing = true;
        return;
    }

    nWithoutEventCounter = 0;

    switch (stateStack[nStackDepth].eVal)
    {
        case STATE_DEFAULT:
            break;
        case STATE_SHEETDATA:
            break;
        case STATE_ROW:
            break;
        case STATE_CELL:
            break;
        case STATE_TEXTV:
            dataHandlerTextV(data, nLen);
            break;
        default:
            break;
    }
}

/************************************************************************/
/*                                PushState()                           */
/************************************************************************/

void OGRXLSXDataSource::PushState(HandlerStateEnum eVal)
{
    if (nStackDepth + 1 == STACK_SIZE)
    {
        bStopParsing = true;
        return;
    }
    nStackDepth++;
    stateStack[nStackDepth].eVal = eVal;
    stateStack[nStackDepth].nBeginDepth = nDepth;
}

/************************************************************************/
/*                          GetAttributeValue()                         */
/************************************************************************/

static const char *GetAttributeValue(const char **ppszAttr, const char *pszKey,
                                     const char *pszDefaultVal)
{
    while (*ppszAttr)
    {
        if (strcmp(ppszAttr[0], pszKey) == 0)
            return ppszAttr[1];
        ppszAttr += 2;
    }
    return pszDefaultVal;
}

/************************************************************************/
/*                            GetOGRFieldType()                         */
/************************************************************************/

OGRFieldType OGRXLSXDataSource::GetOGRFieldType(const char *pszValue,
                                                const char *pszValueType,
                                                OGRFieldSubType &eSubType)
{
    eSubType = OFSTNone;
    if (!bAutodetectTypes || pszValueType == nullptr)
        return OFTString;
    else if (strcmp(pszValueType, "string") == 0)
        return OFTString;
    else if (strcmp(pszValueType, "float") == 0)
    {
        CPLValueType eValueType = CPLGetValueType(pszValue);
        if (eValueType == CPL_VALUE_STRING)
            return OFTString;
        else if (eValueType == CPL_VALUE_INTEGER)
        {
            GIntBig nVal = CPLAtoGIntBig(pszValue);
            if (!CPL_INT64_FITS_ON_INT32(nVal))
                return OFTInteger64;
            else
                return OFTInteger;
        }
        else
            return OFTReal;
    }
    else if (strcmp(pszValueType, "datetime") == 0 ||
             strcmp(pszValueType, "datetime_ms") == 0)
    {
        return OFTDateTime;
    }
    else if (strcmp(pszValueType, "date") == 0)
    {
        return OFTDate;
    }
    else if (strcmp(pszValueType, "time") == 0)
    {
        return OFTTime;
    }
    else if (strcmp(pszValueType, "bool") == 0)
    {
        eSubType = OFSTBoolean;
        return OFTInteger;
    }
    else
        return OFTString;
}

/************************************************************************/
/*                              SetField()                              */
/************************************************************************/

static void SetField(OGRFeature *poFeature, int i, const char *pszValue,
                     const char *pszCellType)
{
    if (pszValue[0] == '\0')
        return;

    OGRFieldType eType = poFeature->GetFieldDefnRef(i)->GetType();

    if (strcmp(pszCellType, "time") == 0 || strcmp(pszCellType, "date") == 0 ||
        strcmp(pszCellType, "datetime") == 0 ||
        strcmp(pszCellType, "datetime_ms") == 0)
    {
        struct tm sTm;
        const double dfNumberOfDaysSince1900 = CPLAtof(pszValue);
        if (!(std::fabs(dfNumberOfDaysSince1900) < 365.0 * 10000))
            return;
        double dfNumberOfSecsSince1900 =
            dfNumberOfDaysSince1900 * NUMBER_OF_SECONDS_PER_DAY;
        if (std::fabs(dfNumberOfSecsSince1900 -
                      std::round(dfNumberOfSecsSince1900)) < 1e-3)
            dfNumberOfSecsSince1900 = std::round(dfNumberOfSecsSince1900);
        const GIntBig nUnixTime =
            static_cast<GIntBig>(dfNumberOfSecsSince1900) -
            static_cast<GIntBig>(NUMBER_OF_DAYS_BETWEEN_1900_AND_1970) *
                NUMBER_OF_SECONDS_PER_DAY;
        CPLUnixTimeToYMDHMS(nUnixTime, &sTm);

        if (eType == OFTTime || eType == OFTDate || eType == OFTDateTime)
        {
            double fFracSec = fmod(dfNumberOfSecsSince1900, 1);
            poFeature->SetField(i, sTm.tm_year + 1900, sTm.tm_mon + 1,
                                sTm.tm_mday, sTm.tm_hour, sTm.tm_min,
                                static_cast<float>(sTm.tm_sec + fFracSec), 0);
        }
        else if (strcmp(pszCellType, "time") == 0)
        {
            poFeature->SetField(i, CPLSPrintf("%02d:%02d:%02d", sTm.tm_hour,
                                              sTm.tm_min, sTm.tm_sec));
        }
        else if (strcmp(pszCellType, "date") == 0)
        {
            poFeature->SetField(i,
                                CPLSPrintf("%04d/%02d/%02d", sTm.tm_year + 1900,
                                           sTm.tm_mon + 1, sTm.tm_mday));
        }
        else /* if (strcmp(pszCellType, "datetime") == 0) */
        {
            double fFracSec = fmod(dfNumberOfSecsSince1900, 1);
            poFeature->SetField(i, sTm.tm_year + 1900, sTm.tm_mon + 1,
                                sTm.tm_mday, sTm.tm_hour, sTm.tm_min,
                                static_cast<float>(sTm.tm_sec + fFracSec), 0);
        }
    }
    else
        poFeature->SetField(i, pszValue);
}

/************************************************************************/
/*                          DetectHeaderLine()                          */
/************************************************************************/

void OGRXLSXDataSource::DetectHeaderLine()

{
    bool bHeaderLineCandidate = true;

    for (size_t i = 0; i < apoFirstLineTypes.size(); i++)
    {
        if (apoFirstLineTypes[i] != "string")
        {
            /* If the values in the first line are not text, then it is */
            /* not a header line */
            bHeaderLineCandidate = false;
            break;
        }
    }

    size_t nCountTextOnCurLine = 0;
    size_t nCountNonEmptyOnCurLine = 0;
    for (size_t i = 0; bHeaderLineCandidate && i < apoCurLineTypes.size(); i++)
    {
        if (apoCurLineTypes[i] == "string")
        {
            /* If there are only text values on the second line, then we cannot
             */
            /* know if it is a header line or just a regular line */
            nCountTextOnCurLine++;
        }
        else if (apoCurLineTypes[i] != "")
        {
            nCountNonEmptyOnCurLine++;
        }
    }

    const char *pszXLSXHeaders =
        CSLFetchNameValueDef(papszOpenOptions, "HEADERS",
                             CPLGetConfigOption("OGR_XLSX_HEADERS", ""));
    bFirstLineIsHeaders = false;
    if (EQUAL(pszXLSXHeaders, "FORCE"))
        bFirstLineIsHeaders = true;
    else if (EQUAL(pszXLSXHeaders, "DISABLE"))
        bFirstLineIsHeaders = false;
    else if (bHeaderLineCandidate && !apoFirstLineTypes.empty() &&
             apoFirstLineTypes.size() >= apoCurLineTypes.size() &&
             nCountTextOnCurLine != apoFirstLineTypes.size() &&
             nCountNonEmptyOnCurLine != 0)
    {
        bFirstLineIsHeaders = true;
    }
    CPLDebug("XLSX", "%s %s", poCurLayer ? poCurLayer->GetName() : "NULL layer",
             bFirstLineIsHeaders ? "has header line" : "has no header line");
}

/************************************************************************/
/*                          startElementDefault()                       */
/************************************************************************/

void OGRXLSXDataSource::startElementDefault(const char *pszNameIn,
                                            CPL_UNUSED const char **ppszAttr)
{
    if (strcmp(pszNameIn, "cols") == 0)
    {
        PushState(STATE_COLS);
        m_osCols = "<cols>";
    }
    else if (strcmp(pszNameIn, "sheetData") == 0)
    {
        apoFirstLineValues.resize(0);
        apoFirstLineTypes.resize(0);
        nCurLine = 0;
        PushState(STATE_SHEETDATA);
    }
}

/************************************************************************/
/*                          startElementCols()                          */
/************************************************************************/

void OGRXLSXDataSource::startElementCols(const char *pszNameIn,
                                         const char **ppszAttr)
{
    m_osCols.append("<");
    m_osCols.append(pszNameIn);
    for (const char **iter = ppszAttr; iter && iter[0] && iter[1]; iter += 2)
    {
        m_osCols.append(" ");
        m_osCols.append(iter[0]);
        m_osCols.append("=\"");
        char *pszXML = OGRGetXML_UTF8_EscapedString(iter[1]);
        m_osCols.append(pszXML);
        CPLFree(pszXML);
        m_osCols.append("\"");
    }
    m_osCols.append(">");
}

/************************************************************************/
/*                            endElementCell()                          */
/************************************************************************/

void OGRXLSXDataSource::endElementCols(const char *pszNameIn)
{
    m_osCols.append("</");
    m_osCols.append(pszNameIn);
    m_osCols.append(">");
}

/************************************************************************/
/*                          startElementTable()                        */
/************************************************************************/

void OGRXLSXDataSource::startElementTable(const char *pszNameIn,
                                          const char **ppszAttr)
{
    if (strcmp(pszNameIn, "row") == 0)
    {
        PushState(STATE_ROW);

        nCurCol = 0;
        apoCurLineValues.clear();
        apoCurLineTypes.clear();

        int nNewCurLine;
        if (const char *pszR = GetAttributeValue(ppszAttr, "r", nullptr))
        {
            nNewCurLine = atoi(pszR);
            if (nNewCurLine <= 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Invalid row: %d",
                         nNewCurLine);
                return;
            }
            nNewCurLine--;
        }
        else
        {
            nNewCurLine = nCurLine;
        }
        const int nFields = std::max(
            static_cast<int>(apoFirstLineValues.size()),
            poCurLayer != nullptr ? poCurLayer->GetLayerDefn()->GetFieldCount()
                                  : 0);
        if (nNewCurLine > nCurLine &&
            (nNewCurLine - nCurLine > 10000 ||
             (nFields > 0 && nNewCurLine - nCurLine > 100000 / nFields)))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid row: %d. Too big gap with previous valid row",
                     nNewCurLine);
            return;
        }
        for (; nCurLine < nNewCurLine;)
        {
            const int nCurLineBefore = nCurLine;
            endElementRow("row");

            nCurCol = 0;
            apoCurLineValues.clear();
            apoCurLineTypes.clear();
            if (nCurLineBefore == nCurLine)
                break;
        }
    }
}

/************************************************************************/
/*                           endElementTable()                          */
/************************************************************************/

void OGRXLSXDataSource::endElementTable(CPL_UNUSED const char *pszNameIn)
{
    if (stateStack[nStackDepth].nBeginDepth == nDepth && poCurLayer != nullptr)
    {
        CPLAssert(strcmp(pszNameIn, "sheetData") == 0);

        if (nCurLine == 0 || (nCurLine == 1 && apoFirstLineValues.empty()))
        {
            // no rows
        }
        else if (nCurLine == 1)
        {
            /* If we have only one single line in the sheet */
            for (size_t i = 0; i < apoFirstLineValues.size(); i++)
            {
                const char *pszFieldName = CPLSPrintf("Field%d", (int)i + 1);
                OGRFieldSubType eSubType = OFSTNone;
                OGRFieldType eType =
                    GetOGRFieldType(apoFirstLineValues[i].c_str(),
                                    apoFirstLineTypes[i].c_str(), eSubType);
                OGRFieldDefn oFieldDefn(pszFieldName, eType);
                oFieldDefn.SetSubType(eSubType);
                if (poCurLayer->CreateField(&oFieldDefn) != OGRERR_NONE)
                {
                    return;
                }
            }

            OGRFeature *poFeature = new OGRFeature(poCurLayer->GetLayerDefn());
            for (size_t i = 0; i < apoFirstLineValues.size(); i++)
            {
                SetField(poFeature, static_cast<int>(i),
                         apoFirstLineValues[i].c_str(),
                         apoFirstLineTypes[i].c_str());
            }
            CPL_IGNORE_RET_VAL(poCurLayer->CreateFeature(poFeature));
            delete poFeature;
        }

        if (poCurLayer)
        {
            poCurLayer->SetUpdatable(CPL_TO_BOOL(bUpdatable));
            poCurLayer->SetUpdated(false);
        }

        poCurLayer = nullptr;
    }
}

/************************************************************************/
/*                            startElementRow()                         */
/************************************************************************/

void OGRXLSXDataSource::startElementRow(const char *pszNameIn,
                                        const char **ppszAttr)
{
    if (strcmp(pszNameIn, "c") == 0)
    {
        PushState(STATE_CELL);

        const char *pszR = GetAttributeValue(ppszAttr, "r", nullptr);
        if (pszR && pszR[0] >= 'A' && pszR[0] <= 'Z')
        {
            /* Convert col number from base 26 */
            /*
            A Z   AA AZ   BA BZ   ZA   ZZ   AAA    ZZZ      AAAA
            0 25  26 51   52 77   676  701  702    18277    18278
            */
            int nNewCurCol = (pszR[0] - 'A');
            int i = 1;
            while (pszR[i] >= 'A' && pszR[i] <= 'Z' && nNewCurCol <= 2000)
            {
                // We wouldn't need the +1 if this was a proper base 26
                nNewCurCol = (nNewCurCol + 1) * 26 + (pszR[i] - 'A');
                i++;
            }
            if (nNewCurCol > 2000)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Limiting number of columns to 2000");
                nNewCurCol = 2000;
            }
            for (; nCurCol < nNewCurCol; nCurCol++)
            {
                apoCurLineValues.push_back("");
                apoCurLineTypes.push_back("");
            }
        }

        osValueType = "float";

        const char *pszS = GetAttributeValue(ppszAttr, "s", "-1");
        int nS = atoi(pszS);
        if (nS >= 0 && nS < (int)apoStyles.size())
        {
            XLSXFieldTypeExtended eType = apoStyles[nS];
            if (eType.eType == OFTDateTime)
            {
                if (eType.bHasMS)
                    osValueType = "datetime_ms";
                else
                    osValueType = "datetime";
            }
            else if (eType.eType == OFTDate)
                osValueType = "date";
            else if (eType.eType == OFTTime)
                osValueType = "time";
        }
        else if (nS != -1)
            CPLDebug("XLSX", "Cannot find style %d", nS);

        const char *pszT = GetAttributeValue(ppszAttr, "t", "");
        if (EQUAL(pszT, "s"))
            osValueType = "stringLookup";
        else if (EQUAL(pszT, "inlineStr"))
            osValueType = "string";
        else if (EQUAL(pszT, "b"))
            osValueType = "bool";

        osValue = "";
    }
}

/************************************************************************/
/*                            endElementRow()                           */
/************************************************************************/

void OGRXLSXDataSource::endElementRow(CPL_UNUSED const char *pszNameIn)
{
    if (stateStack[nStackDepth].nBeginDepth == nDepth && poCurLayer != nullptr)
    {
        CPLAssert(strcmp(pszNameIn, "row") == 0);

        /* Backup first line values and types in special arrays */
        if (nCurLine == 0)
        {
            apoFirstLineTypes = apoCurLineTypes;
            apoFirstLineValues = apoCurLineValues;

#if skip_leading_empty_rows
            if (apoFirstLineTypes.empty())
            {
                /* Skip leading empty rows */
                apoFirstLineTypes.resize(0);
                apoFirstLineValues.resize(0);
                return;
            }
#endif
        }

        if (nCurLine == 1)
        {
            DetectHeaderLine();

            poCurLayer->SetHasHeaderLine(bFirstLineIsHeaders);

            if (bFirstLineIsHeaders)
            {
                for (size_t i = 0; i < apoFirstLineValues.size(); i++)
                {
                    const char *pszFieldName = apoFirstLineValues[i].c_str();
                    if (pszFieldName[0] == '\0')
                        pszFieldName = CPLSPrintf("Field%d", (int)i + 1);
                    bool bUnknownType = true;
                    OGRFieldType eType = OFTString;
                    OGRFieldSubType eSubType = OFSTNone;
                    if (i < apoCurLineValues.size() &&
                        !apoCurLineValues[i].empty())
                    {
                        eType = GetOGRFieldType(apoCurLineValues[i].c_str(),
                                                apoCurLineTypes[i].c_str(),
                                                eSubType);
                        bUnknownType = false;
                    }
                    OGRFieldDefn oFieldDefn(pszFieldName, eType);
                    oFieldDefn.SetSubType(eSubType);
                    if (bUnknownType)
                    {
                        poCurLayer->oSetFieldsOfUnknownType.insert(
                            poCurLayer->GetLayerDefn()->GetFieldCount());
                    }
                    if (poCurLayer->CreateField(&oFieldDefn) != OGRERR_NONE)
                    {
                        return;
                    }
                }
            }
            else
            {
                for (size_t i = 0; i < apoFirstLineValues.size(); i++)
                {
                    const char *pszFieldName =
                        CPLSPrintf("Field%d", (int)i + 1);
                    OGRFieldSubType eSubType = OFSTNone;
                    OGRFieldType eType =
                        GetOGRFieldType(apoFirstLineValues[i].c_str(),
                                        apoFirstLineTypes[i].c_str(), eSubType);
                    OGRFieldDefn oFieldDefn(pszFieldName, eType);
                    oFieldDefn.SetSubType(eSubType);
                    if (poCurLayer->CreateField(&oFieldDefn) != OGRERR_NONE)
                    {
                        return;
                    }
                }

                OGRFeature *poFeature =
                    new OGRFeature(poCurLayer->GetLayerDefn());
                for (size_t i = 0; i < apoFirstLineValues.size(); i++)
                {
                    SetField(poFeature, static_cast<int>(i),
                             apoFirstLineValues[i].c_str(),
                             apoFirstLineTypes[i].c_str());
                }
                CPL_IGNORE_RET_VAL(poCurLayer->CreateFeature(poFeature));
                delete poFeature;
            }
        }

        if (nCurLine >= 1)
        {
            /* Add new fields found on following lines. */
            if (apoCurLineValues.size() >
                (size_t)poCurLayer->GetLayerDefn()->GetFieldCount())
            {
                GIntBig nFeatureCount = poCurLayer->GetFeatureCount(false);
                if (nFeatureCount > 0 &&
                    static_cast<size_t>(
                        apoCurLineValues.size() -
                        poCurLayer->GetLayerDefn()->GetFieldCount()) >
                        static_cast<size_t>(100000 / nFeatureCount))
                {
                    CPLError(CE_Failure, CPLE_NotSupported,
                             "Adding too many columns to too many "
                             "existing features");
                    return;
                }
                for (size_t i =
                         (size_t)poCurLayer->GetLayerDefn()->GetFieldCount();
                     i < apoCurLineValues.size(); i++)
                {
                    const char *pszFieldName =
                        CPLSPrintf("Field%d", (int)i + 1);
                    OGRFieldSubType eSubType = OFSTNone;
                    OGRFieldType eType =
                        GetOGRFieldType(apoCurLineValues[i].c_str(),
                                        apoCurLineTypes[i].c_str(), eSubType);
                    OGRFieldDefn oFieldDefn(pszFieldName, eType);
                    oFieldDefn.SetSubType(eSubType);
                    if (poCurLayer->CreateField(&oFieldDefn) != OGRERR_NONE)
                    {
                        return;
                    }
                }
            }

            /* Update field type if necessary */
            if (bAutodetectTypes)
            {
                for (size_t i = 0; i < apoCurLineValues.size(); i++)
                {
                    if (!apoCurLineValues[i].empty())
                    {
                        OGRFieldSubType eValSubType = OFSTNone;
                        OGRFieldType eValType = GetOGRFieldType(
                            apoCurLineValues[i].c_str(),
                            apoCurLineTypes[i].c_str(), eValSubType);
                        OGRFieldDefn *poFieldDefn =
                            poCurLayer->GetLayerDefn()->GetFieldDefn(
                                static_cast<int>(i));
                        const OGRFieldType eFieldType = poFieldDefn->GetType();
                        auto oIter = poCurLayer->oSetFieldsOfUnknownType.find(
                            static_cast<int>(i));
                        if (oIter != poCurLayer->oSetFieldsOfUnknownType.end())
                        {
                            poCurLayer->oSetFieldsOfUnknownType.erase(oIter);

                            auto oTemporaryUnsealer(
                                poFieldDefn->GetTemporaryUnsealer());
                            poFieldDefn->SetType(eValType);
                            poFieldDefn->SetSubType(eValSubType);
                        }
                        else if (eFieldType == OFTDateTime &&
                                 (eValType == OFTDate || eValType == OFTTime))
                        {
                            /* ok */
                        }
                        else if (eFieldType == OFTReal &&
                                 (eValType == OFTInteger ||
                                  eValType == OFTInteger64))
                        {
                            /* ok */;
                        }
                        else if (eFieldType == OFTInteger64 &&
                                 eValType == OFTInteger)
                        {
                            /* ok */;
                        }
                        else if (eFieldType != OFTString &&
                                 eValType != eFieldType)
                        {
                            OGRFieldDefn oNewFieldDefn(poFieldDefn);
                            oNewFieldDefn.SetSubType(OFSTNone);
                            if ((eFieldType == OFTDate ||
                                 eFieldType == OFTTime) &&
                                eValType == OFTDateTime)
                                oNewFieldDefn.SetType(OFTDateTime);
                            else if ((eFieldType == OFTInteger ||
                                      eFieldType == OFTInteger64) &&
                                     eValType == OFTReal)
                                oNewFieldDefn.SetType(OFTReal);
                            else if (eFieldType == OFTInteger &&
                                     eValType == OFTInteger64)
                                oNewFieldDefn.SetType(OFTInteger64);
                            else
                                oNewFieldDefn.SetType(OFTString);
                            poCurLayer->AlterFieldDefn(static_cast<int>(i),
                                                       &oNewFieldDefn,
                                                       ALTER_TYPE_FLAG);
                        }
                        else if (eFieldType == OFTInteger &&
                                 poFieldDefn->GetSubType() == OFSTBoolean &&
                                 eValType == OFTInteger &&
                                 eValSubType != OFSTBoolean)
                        {
                            poFieldDefn->SetSubType(OFSTNone);
                        }
                    }
                }
            }

            /* Add feature for current line */
            OGRFeature *poFeature = new OGRFeature(poCurLayer->GetLayerDefn());
            for (size_t i = 0; i < apoCurLineValues.size(); i++)
            {
                if (!apoCurLineValues[i].empty())
                {
                    SetField(poFeature, static_cast<int>(i),
                             apoCurLineValues[i].c_str(),
                             apoCurLineTypes[i].c_str());
                }
            }
            CPL_IGNORE_RET_VAL(poCurLayer->CreateFeature(poFeature));
            delete poFeature;
        }

        nCurLine++;
    }
}

/************************************************************************/
/*                           startElementCell()                         */
/************************************************************************/

void OGRXLSXDataSource::startElementCell(const char *pszNameIn,
                                         CPL_UNUSED const char **ppszAttr)
{
    if (osValue.empty() && strcmp(pszNameIn, "v") == 0)
    {
        PushState(STATE_TEXTV);
    }
    else if (osValue.empty() && strcmp(pszNameIn, "t") == 0)
    {
        PushState(STATE_TEXTV);
    }
}

/************************************************************************/
/*                            endElementCell()                          */
/************************************************************************/

void OGRXLSXDataSource::endElementCell(CPL_UNUSED const char *pszNameIn)
{
    if (stateStack[nStackDepth].nBeginDepth == nDepth)
    {
        CPLAssert(strcmp(pszNameIn, "c") == 0);

        if (osValueType == "stringLookup")
        {
            int nIndex = atoi(osValue);
            if (nIndex >= 0 && nIndex < (int)(apoSharedStrings.size()))
                osValue = apoSharedStrings[nIndex];
            else
                CPLDebug("XLSX", "Cannot find string %d", nIndex);
            osValueType = "string";
        }

        apoCurLineValues.push_back(osValue);
        apoCurLineTypes.push_back(osValueType);

        nCurCol += 1;
    }
}

/************************************************************************/
/*                           dataHandlerTextV()                         */
/************************************************************************/

void OGRXLSXDataSource::dataHandlerTextV(const char *data, int nLen)
{
    osValue.append(data, nLen);
}

/************************************************************************/
/*                              BuildLayer()                            */
/************************************************************************/

void OGRXLSXDataSource::BuildLayer(OGRXLSXLayer *poLayer)
{
    poCurLayer = poLayer;

    const char *pszSheetFilename = poLayer->GetFilename().c_str();
    VSILFILE *fp = VSIFOpenL(pszSheetFilename, "rb");
    if (fp == nullptr)
    {
        CPLDebug("XLSX", "Cannot open file %s for sheet %s", pszSheetFilename,
                 poLayer->GetName());
        return;
    }

    const bool bUpdatedBackup = bUpdated;

    oParser = OGRCreateExpatXMLParser();
    m_osCols.clear();
    XML_SetElementHandler(oParser, OGRXLSX::startElementCbk,
                          OGRXLSX::endElementCbk);
    XML_SetCharacterDataHandler(oParser, OGRXLSX::dataHandlerCbk);
    XML_SetUserData(oParser, this);

    VSIFSeekL(fp, 0, SEEK_SET);

    bStopParsing = false;
    nWithoutEventCounter = 0;
    nDataHandlerCounter = 0;
    nStackDepth = 0;
    nDepth = 0;
    stateStack[0].eVal = STATE_DEFAULT;
    stateStack[0].nBeginDepth = 0;

    std::vector<char> aBuf(PARSER_BUF_SIZE);
    int nDone = 0;
    do
    {
        nDataHandlerCounter = 0;
        unsigned int nLen =
            (unsigned int)VSIFReadL(aBuf.data(), 1, aBuf.size(), fp);
        nDone = (nLen < aBuf.size());
        if (XML_Parse(oParser, aBuf.data(), nLen, nDone) == XML_STATUS_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "XML parsing of %s file failed : %s at line %d, column %d",
                     pszSheetFilename,
                     XML_ErrorString(XML_GetErrorCode(oParser)),
                     (int)XML_GetCurrentLineNumber(oParser),
                     (int)XML_GetCurrentColumnNumber(oParser));
            bStopParsing = true;
        }
        nWithoutEventCounter++;
    } while (!nDone && !bStopParsing && nWithoutEventCounter < 10);

    XML_ParserFree(oParser);
    oParser = nullptr;

    if (nWithoutEventCounter == 10)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Too much data inside one element. File probably corrupted");
        bStopParsing = true;
    }

    VSIFCloseL(fp);

    bUpdated = bUpdatedBackup;
    poLayer->m_osCols = m_osCols;
}

/************************************************************************/
/*                          startElementSSCbk()                         */
/************************************************************************/

static void XMLCALL startElementSSCbk(void *pUserData, const char *pszNameIn,
                                      const char **ppszAttr)
{
    ((OGRXLSXDataSource *)pUserData)->startElementSSCbk(pszNameIn, ppszAttr);
}

void OGRXLSXDataSource::startElementSSCbk(const char *pszNameIn,
                                          CPL_UNUSED const char **ppszAttr)
{
    if (bStopParsing)
        return;

    pszNameIn = GetUnprefixed(pszNameIn);

    nWithoutEventCounter = 0;
    switch (stateStack[nStackDepth].eVal)
    {
        case STATE_DEFAULT:
        {
            if (strcmp(pszNameIn, "si") == 0)
            {
                PushState(STATE_SI);
                osCurrentString = "";
            }
            break;
        }
        case STATE_SI:
        {
            if (strcmp(pszNameIn, "t") == 0)
            {
                PushState(STATE_T);
            }
            break;
        }
        default:
            break;
    }
    nDepth++;
}

/************************************************************************/
/*                           endElementSSCbk()                          */
/************************************************************************/

static void XMLCALL endElementSSCbk(void *pUserData, const char *pszNameIn)
{
    ((OGRXLSXDataSource *)pUserData)->endElementSSCbk(pszNameIn);
}

void OGRXLSXDataSource::endElementSSCbk(const char * /*pszNameIn*/)
{
    if (bStopParsing)
        return;

    // If we were to use pszNameIn, then we need:
    // pszNameIn = GetUnprefixed(pszNameIn);

    nWithoutEventCounter = 0;

    nDepth--;
    switch (stateStack[nStackDepth].eVal)
    {
        case STATE_DEFAULT:
            break;
        case STATE_T:
            break;
        case STATE_SI:
        {
            if (stateStack[nStackDepth].nBeginDepth == nDepth)
            {
                apoSharedStrings.push_back(osCurrentString);
            }
            break;
        }
        default:
            break;
    }

    if (stateStack[nStackDepth].nBeginDepth == nDepth)
        nStackDepth--;
}

/************************************************************************/
/*                           dataHandlerSSCbk()                         */
/************************************************************************/

static void XMLCALL dataHandlerSSCbk(void *pUserData, const char *data,
                                     int nLen)
{
    ((OGRXLSXDataSource *)pUserData)->dataHandlerSSCbk(data, nLen);
}

void OGRXLSXDataSource::dataHandlerSSCbk(const char *data, int nLen)
{
    if (bStopParsing)
        return;

    nDataHandlerCounter++;
    if (nDataHandlerCounter >= PARSER_BUF_SIZE)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "File probably corrupted (million laugh pattern)");
        XML_StopParser(oParser, XML_FALSE);
        bStopParsing = true;
        return;
    }

    nWithoutEventCounter = 0;

    switch (stateStack[nStackDepth].eVal)
    {
        case STATE_DEFAULT:
            break;
        case STATE_SI:
            break;
        case STATE_T:
            osCurrentString.append(data, nLen);
            break;
        default:
            break;
    }
}

/************************************************************************/
/*                          AnalyseSharedStrings()                      */
/************************************************************************/

void OGRXLSXDataSource::AnalyseSharedStrings(VSILFILE *fpSharedStrings)
{
    if (fpSharedStrings == nullptr)
        return;

    oParser = OGRCreateExpatXMLParser();
    XML_SetElementHandler(oParser, OGRXLSX::startElementSSCbk,
                          OGRXLSX::endElementSSCbk);
    XML_SetCharacterDataHandler(oParser, OGRXLSX::dataHandlerSSCbk);
    XML_SetUserData(oParser, this);

    VSIFSeekL(fpSharedStrings, 0, SEEK_SET);

    bStopParsing = false;
    nWithoutEventCounter = 0;
    nDataHandlerCounter = 0;
    nStackDepth = 0;
    nDepth = 0;
    stateStack[0].eVal = STATE_DEFAULT;
    stateStack[0].nBeginDepth = 0;

    std::vector<char> aBuf(PARSER_BUF_SIZE);
    int nDone = 0;
    do
    {
        nDataHandlerCounter = 0;
        unsigned int nLen = (unsigned int)VSIFReadL(aBuf.data(), 1, aBuf.size(),
                                                    fpSharedStrings);
        nDone = (nLen < aBuf.size());
        if (XML_Parse(oParser, aBuf.data(), nLen, nDone) == XML_STATUS_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "XML parsing of %s file failed : %s at line %d, column %d",
                     "sharedStrings.xml",
                     XML_ErrorString(XML_GetErrorCode(oParser)),
                     (int)XML_GetCurrentLineNumber(oParser),
                     (int)XML_GetCurrentColumnNumber(oParser));
            bStopParsing = true;
        }
        nWithoutEventCounter++;
    } while (!nDone && !bStopParsing && nWithoutEventCounter < 10);

    XML_ParserFree(oParser);
    oParser = nullptr;

    if (nWithoutEventCounter == 10)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Too much data inside one element. File probably corrupted");
        bStopParsing = true;
    }

    VSIFCloseL(fpSharedStrings);
}

/************************************************************************/
/*                        startElementWBRelsCbk()                       */
/************************************************************************/

static void XMLCALL startElementWBRelsCbk(void *pUserData,
                                          const char *pszNameIn,
                                          const char **ppszAttr)
{
    ((OGRXLSXDataSource *)pUserData)
        ->startElementWBRelsCbk(pszNameIn, ppszAttr);
}

void OGRXLSXDataSource::startElementWBRelsCbk(const char *pszNameIn,
                                              const char **ppszAttr)
{
    if (bStopParsing)
        return;

    pszNameIn = GetUnprefixed(pszNameIn);

    nWithoutEventCounter = 0;
    if (strcmp(pszNameIn, "Relationship") == 0)
    {
        const char *pszId = GetAttributeValue(ppszAttr, "Id", nullptr);
        const char *pszType = GetAttributeValue(ppszAttr, "Type", nullptr);
        const char *pszTarget = GetAttributeValue(ppszAttr, "Target", nullptr);
        if (pszId && pszType && pszTarget &&
            strstr(pszType, "/worksheet") != nullptr)
        {
            oMapRelsIdToTarget[pszId] = pszTarget;
        }
    }
}

/************************************************************************/
/*                          AnalyseWorkbookRels()                       */
/************************************************************************/

void OGRXLSXDataSource::AnalyseWorkbookRels(VSILFILE *fpWorkbookRels)
{
    oParser = OGRCreateExpatXMLParser();
    XML_SetElementHandler(oParser, OGRXLSX::startElementWBRelsCbk, nullptr);
    XML_SetUserData(oParser, this);

    VSIFSeekL(fpWorkbookRels, 0, SEEK_SET);

    bStopParsing = false;
    nWithoutEventCounter = 0;
    nDataHandlerCounter = 0;

    std::vector<char> aBuf(PARSER_BUF_SIZE);
    int nDone = 0;
    do
    {
        nDataHandlerCounter = 0;
        unsigned int nLen = (unsigned int)VSIFReadL(aBuf.data(), 1, aBuf.size(),
                                                    fpWorkbookRels);
        nDone = (nLen < aBuf.size());
        if (XML_Parse(oParser, aBuf.data(), nLen, nDone) == XML_STATUS_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "XML parsing of %s file failed : %s at line %d, column %d",
                     "xl/_rels/workbook.xml.rels",
                     XML_ErrorString(XML_GetErrorCode(oParser)),
                     (int)XML_GetCurrentLineNumber(oParser),
                     (int)XML_GetCurrentColumnNumber(oParser));
            bStopParsing = true;
        }
        nWithoutEventCounter++;
    } while (!nDone && !bStopParsing && nWithoutEventCounter < 10);

    XML_ParserFree(oParser);
    oParser = nullptr;

    if (nWithoutEventCounter == 10)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Too much data inside one element. File probably corrupted");
        bStopParsing = true;
    }

    VSIFCloseL(fpWorkbookRels);
}

/************************************************************************/
/*                          startElementWBCbk()                         */
/************************************************************************/

static void XMLCALL startElementWBCbk(void *pUserData, const char *pszNameIn,
                                      const char **ppszAttr)
{
    ((OGRXLSXDataSource *)pUserData)->startElementWBCbk(pszNameIn, ppszAttr);
}

void OGRXLSXDataSource::startElementWBCbk(const char *pszNameIn,
                                          const char **ppszAttr)
{
    if (bStopParsing)
        return;

    pszNameIn = GetUnprefixed(pszNameIn);

    nWithoutEventCounter = 0;
    if (strcmp(pszNameIn, "sheet") == 0)
    {
        const char *pszSheetName = GetAttributeValue(ppszAttr, "name", nullptr);
        const char *pszId = GetAttributeValue(ppszAttr, "r:id", nullptr);
        if (pszSheetName && pszId &&
            oMapRelsIdToTarget.find(pszId) != oMapRelsIdToTarget.end() &&
            m_oSetSheetId.find(pszId) == m_oSetSheetId.end())
        {
            const auto &osTarget(oMapRelsIdToTarget[pszId]);
            m_oSetSheetId.insert(pszId);
            CPLString osFilename;
            if (osTarget.empty())
                return;
            if (osTarget[0] == '/')
            {
                int nIdx = 1;
                while (osTarget[nIdx] == '/')
                    nIdx++;
                if (osTarget[nIdx] == '\0')
                    return;
                // Is it an "absolute" path ?
                osFilename = osPrefixedFilename + osTarget;
            }
            else
            {
                // or relative to the /xl subdirectory
                osFilename = osPrefixedFilename + CPLString("/xl/") + osTarget;
            }
            papoLayers = (OGRXLSXLayer **)CPLRealloc(
                papoLayers, (nLayers + 1) * sizeof(OGRXLSXLayer *));
            papoLayers[nLayers++] =
                new OGRXLSXLayer(this, osFilename, pszSheetName);
        }
    }
}

/************************************************************************/
/*                             AnalyseWorkbook()                        */
/************************************************************************/

void OGRXLSXDataSource::AnalyseWorkbook(VSILFILE *fpWorkbook)
{
    oParser = OGRCreateExpatXMLParser();
    XML_SetElementHandler(oParser, OGRXLSX::startElementWBCbk, nullptr);
    XML_SetUserData(oParser, this);

    VSIFSeekL(fpWorkbook, 0, SEEK_SET);

    bStopParsing = false;
    nWithoutEventCounter = 0;
    nDataHandlerCounter = 0;

    std::vector<char> aBuf(PARSER_BUF_SIZE);
    int nDone = 0;
    do
    {
        nDataHandlerCounter = 0;
        unsigned int nLen =
            (unsigned int)VSIFReadL(aBuf.data(), 1, aBuf.size(), fpWorkbook);
        nDone = (nLen < aBuf.size());
        if (XML_Parse(oParser, aBuf.data(), nLen, nDone) == XML_STATUS_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "XML parsing of %s file failed : %s at line %d, column %d",
                     "workbook.xml", XML_ErrorString(XML_GetErrorCode(oParser)),
                     (int)XML_GetCurrentLineNumber(oParser),
                     (int)XML_GetCurrentColumnNumber(oParser));
            bStopParsing = true;
        }
        nWithoutEventCounter++;
    } while (!nDone && !bStopParsing && nWithoutEventCounter < 10);

    XML_ParserFree(oParser);
    oParser = nullptr;

    if (nWithoutEventCounter == 10)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Too much data inside one element. File probably corrupted");
        bStopParsing = true;
    }

    VSIFCloseL(fpWorkbook);
}

/************************************************************************/
/*                       startElementStylesCbk()                        */
/************************************************************************/

static void XMLCALL startElementStylesCbk(void *pUserData,
                                          const char *pszNameIn,
                                          const char **ppszAttr)
{
    ((OGRXLSXDataSource *)pUserData)
        ->startElementStylesCbk(pszNameIn, ppszAttr);
}

void OGRXLSXDataSource::startElementStylesCbk(const char *pszNameIn,
                                              const char **ppszAttr)
{
    if (bStopParsing)
        return;

    pszNameIn = GetUnprefixed(pszNameIn);

    nWithoutEventCounter = 0;
    if (strcmp(pszNameIn, "numFmt") == 0)
    {
        const char *pszFormatCode =
            GetAttributeValue(ppszAttr, "formatCode", nullptr);
        const char *pszNumFmtId = GetAttributeValue(ppszAttr, "numFmtId", "-1");
        int nNumFmtId = atoi(pszNumFmtId);
        if (pszFormatCode && nNumFmtId >= 164)
        {
            int bHasDate = strstr(pszFormatCode, "DD") != nullptr ||
                           strstr(pszFormatCode, "dd") != nullptr ||
                           strstr(pszFormatCode, "YY") != nullptr ||
                           strstr(pszFormatCode, "yy") != nullptr;
            int bHasTime = strstr(pszFormatCode, "HH") != nullptr ||
                           strstr(pszFormatCode, "hh") != nullptr;
            if (bHasDate && bHasTime)
                apoMapStyleFormats[nNumFmtId] = XLSXFieldTypeExtended(
                    OFTDateTime,
                    strstr(pszFormatCode, "SS.000") != nullptr ||
                        strstr(pszFormatCode, "ss.000") != nullptr);
            else if (bHasDate)
                apoMapStyleFormats[nNumFmtId] = XLSXFieldTypeExtended(OFTDate);
            else if (bHasTime)
                apoMapStyleFormats[nNumFmtId] = XLSXFieldTypeExtended(OFTTime);
            else
                apoMapStyleFormats[nNumFmtId] = XLSXFieldTypeExtended(OFTReal);
        }
    }
    else if (strcmp(pszNameIn, "cellXfs") == 0)
    {
        bInCellXFS = true;
    }
    else if (bInCellXFS && strcmp(pszNameIn, "xf") == 0)
    {
        const char *pszNumFmtId = GetAttributeValue(ppszAttr, "numFmtId", "-1");
        int nNumFmtId = atoi(pszNumFmtId);
        XLSXFieldTypeExtended eType(OFTReal);
        if (nNumFmtId >= 0)
        {
            if (nNumFmtId < 164)
            {
                // From
                // http://social.msdn.microsoft.com/Forums/en-US/oxmlsdk/thread/e27aaf16-b900-4654-8210-83c5774a179c/
                if (nNumFmtId >= 14 && nNumFmtId <= 17)
                    eType = XLSXFieldTypeExtended(OFTDate);
                else if (nNumFmtId >= 18 && nNumFmtId <= 21)
                    eType = XLSXFieldTypeExtended(OFTTime);
                else if (nNumFmtId == 22)
                    eType = XLSXFieldTypeExtended(OFTDateTime);
            }
            else
            {
                std::map<int, XLSXFieldTypeExtended>::iterator oIter =
                    apoMapStyleFormats.find(nNumFmtId);
                if (oIter != apoMapStyleFormats.end())
                    eType = oIter->second;
                else
                    CPLDebug("XLSX",
                             "Cannot find entry in <numFmts> with numFmtId=%d",
                             nNumFmtId);
            }
        }
#if DEBUG_VERBOSE
        printf("style[%lu] = %d\n", /*ok*/
               apoStyles.size(), static_cast<int>(eType.eType));
#endif

        apoStyles.push_back(eType);
    }
}

/************************************************************************/
/*                       endElementStylesCbk()                          */
/************************************************************************/

static void XMLCALL endElementStylesCbk(void *pUserData, const char *pszNameIn)
{
    ((OGRXLSXDataSource *)pUserData)->endElementStylesCbk(pszNameIn);
}

void OGRXLSXDataSource::endElementStylesCbk(const char *pszNameIn)
{
    if (bStopParsing)
        return;

    pszNameIn = GetUnprefixed(pszNameIn);

    nWithoutEventCounter = 0;
    if (strcmp(pszNameIn, "cellXfs") == 0)
    {
        bInCellXFS = false;
    }
}

/************************************************************************/
/*                             AnalyseStyles()                          */
/************************************************************************/

void OGRXLSXDataSource::AnalyseStyles(VSILFILE *fpStyles)
{
    if (fpStyles == nullptr)
        return;

    oParser = OGRCreateExpatXMLParser();
    XML_SetElementHandler(oParser, OGRXLSX::startElementStylesCbk,
                          OGRXLSX::endElementStylesCbk);
    XML_SetUserData(oParser, this);

    VSIFSeekL(fpStyles, 0, SEEK_SET);

    bStopParsing = false;
    nWithoutEventCounter = 0;
    nDataHandlerCounter = 0;
    bInCellXFS = false;

    std::vector<char> aBuf(PARSER_BUF_SIZE);
    int nDone = 0;
    do
    {
        nDataHandlerCounter = 0;
        unsigned int nLen =
            (unsigned int)VSIFReadL(aBuf.data(), 1, aBuf.size(), fpStyles);
        nDone = (nLen < aBuf.size());
        if (XML_Parse(oParser, aBuf.data(), nLen, nDone) == XML_STATUS_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "XML parsing of %s file failed : %s at line %d, column %d",
                     "styles.xml", XML_ErrorString(XML_GetErrorCode(oParser)),
                     (int)XML_GetCurrentLineNumber(oParser),
                     (int)XML_GetCurrentColumnNumber(oParser));
            bStopParsing = true;
        }
        nWithoutEventCounter++;
    } while (!nDone && !bStopParsing && nWithoutEventCounter < 10);

    XML_ParserFree(oParser);
    oParser = nullptr;

    if (nWithoutEventCounter == 10)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Too much data inside one element. File probably corrupted");
        bStopParsing = true;
    }

    VSIFCloseL(fpStyles);
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *
OGRXLSXDataSource::ICreateLayer(const char *pszLayerName,
                                const OGRGeomFieldDefn * /*poGeomFieldDefn*/,
                                CSLConstList papszOptions)

{
    /* -------------------------------------------------------------------- */
    /*      Verify we are in update mode.                                   */
    /* -------------------------------------------------------------------- */
    if (!bUpdatable)
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                 "Data source %s opened read-only.\n"
                 "New layer %s cannot be created.\n",
                 pszName, pszLayerName);

        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Do we already have this layer?  If so, should we blow it        */
    /*      away?                                                           */
    /* -------------------------------------------------------------------- */
    for (int iLayer = 0; iLayer < nLayers; iLayer++)
    {
        if (EQUAL(pszLayerName, papoLayers[iLayer]->GetName()))
        {
            if (CSLFetchNameValue(papszOptions, "OVERWRITE") != nullptr &&
                !EQUAL(CSLFetchNameValue(papszOptions, "OVERWRITE"), "NO"))
            {
                DeleteLayer(pszLayerName);
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Layer %s already exists, CreateLayer failed.\n"
                         "Use the layer creation option OVERWRITE=YES to "
                         "replace it.",
                         pszLayerName);
                return nullptr;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Create the layer object.                                        */
    /* -------------------------------------------------------------------- */
    OGRXLSXLayer *poLayer =
        new OGRXLSXLayer(this,
                         CPLSPrintf("/vsizip/%s/xl/worksheets/sheet%d.xml",
                                    pszName, nLayers + 1),
                         pszLayerName, TRUE);

    papoLayers = (OGRXLSXLayer **)CPLRealloc(
        papoLayers, (nLayers + 1) * sizeof(OGRXLSXLayer *));
    papoLayers[nLayers] = poLayer;
    nLayers++;

    bUpdated = true;

    return poLayer;
}

/************************************************************************/
/*                            DeleteLayer()                             */
/************************************************************************/

void OGRXLSXDataSource::DeleteLayer(const char *pszLayerName)

{
    /* -------------------------------------------------------------------- */
    /*      Verify we are in update mode.                                   */
    /* -------------------------------------------------------------------- */
    if (!bUpdatable)
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                 "Data source %s opened read-only.\n"
                 "Layer %s cannot be deleted.\n",
                 pszName, pszLayerName);

        return;
    }

    /* -------------------------------------------------------------------- */
    /*      Try to find layer.                                              */
    /* -------------------------------------------------------------------- */
    int iLayer = 0;
    for (; iLayer < nLayers; iLayer++)
    {
        if (EQUAL(pszLayerName, papoLayers[iLayer]->GetName()))
            break;
    }

    if (iLayer == nLayers)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to delete layer '%s', but this layer is not known to OGR.",
            pszLayerName);
        return;
    }

    DeleteLayer(iLayer);
}

/************************************************************************/
/*                            DeleteLayer()                             */
/************************************************************************/

OGRErr OGRXLSXDataSource::DeleteLayer(int iLayer)
{
    if (iLayer < 0 || iLayer >= nLayers)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Layer %d not in legal range of 0 to %d.", iLayer,
                 nLayers - 1);
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Blow away our OGR structures related to the layer.  This is     */
    /*      pretty dangerous if anything has a reference to this layer!     */
    /* -------------------------------------------------------------------- */

    delete papoLayers[iLayer];
    memmove(papoLayers + iLayer, papoLayers + iLayer + 1,
            sizeof(void *) * (nLayers - iLayer - 1));
    nLayers--;

    bUpdated = true;

    return OGRERR_NONE;
}

/************************************************************************/
/*                            WriteOverride()                           */
/************************************************************************/

static void WriteOverride(VSILFILE *fp, const char *pszPartName,
                          const char *pszContentType)
{
    VSIFPrintfL(fp, "<Override PartName=\"%s\" ContentType=\"%s\"/>\n",
                pszPartName, pszContentType);
}

static const char XML_HEADER[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
static const char MAIN_NS[] =
    "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"";
static const char SCHEMA_OD[] =
    "http://schemas.openxmlformats.org/officeDocument/2006";
static const char SCHEMA_OD_RS[] =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
static const char SCHEMA_PACKAGE[] =
    "http://schemas.openxmlformats.org/package/2006";
static const char SCHEMA_PACKAGE_RS[] =
    "http://schemas.openxmlformats.org/package/2006/relationships";

/************************************************************************/
/*                           WriteContentTypes()                        */
/************************************************************************/

static bool WriteContentTypes(const char *pszName, int nLayers)
{
    CPLString osTmpFilename(
        CPLSPrintf("/vsizip/%s/[Content_Types].xml", pszName));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    // TODO(schwehr): Convert all strlen(XML_HEADER) to constexpr with
    // switch to C++11 or newer.
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp, "<Types xmlns=\"%s/content-types\">\n", SCHEMA_PACKAGE);
    WriteOverride(fp, "/_rels/.rels",
                  "application/vnd.openxmlformats-package.relationships+xml");
    WriteOverride(fp, "/docProps/core.xml",
                  "application/vnd.openxmlformats-package.core-properties+xml");
    WriteOverride(fp, "/docProps/app.xml",
                  "application/"
                  "vnd.openxmlformats-officedocument.extended-properties+xml");
    WriteOverride(fp, "/xl/_rels/workbook.xml.rels",
                  "application/vnd.openxmlformats-package.relationships+xml");
    for (int i = 0; i < nLayers; i++)
    {
        WriteOverride(
            fp, CPLSPrintf("/xl/worksheets/sheet%d.xml", i + 1),
            "application/"
            "vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml");
    }
    WriteOverride(fp, "/xl/styles.xml",
                  "application/"
                  "vnd.openxmlformats-officedocument.spreadsheetml.styles+xml");
    WriteOverride(
        fp, "/xl/workbook.xml",
        "application/"
        "vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml");
    WriteOverride(
        fp, "/xl/sharedStrings.xml",
        "application/"
        "vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml");
    VSIFPrintfL(fp, "</Types>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                             WriteApp()                               */
/************************************************************************/

static bool WriteApp(const char *pszName)
{
    CPLString osTmpFilename(CPLSPrintf("/vsizip/%s/docProps/app.xml", pszName));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp,
                "<Properties xmlns=\"%s/extended-properties\" "
                "xmlns:vt=\"%s/docPropsVTypes\">\n",
                SCHEMA_OD, SCHEMA_OD);
    VSIFPrintfL(fp, "<TotalTime>0</TotalTime>\n");
    VSIFPrintfL(fp, "</Properties>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                             WriteCore()                              */
/************************************************************************/

static bool WriteCore(const char *pszName)
{
    CPLString osTmpFilename(
        CPLSPrintf("/vsizip/%s/docProps/core.xml", pszName));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp,
                "<cp:coreProperties xmlns:cp=\"%s/metadata/core-properties\" "
                "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                "xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" "
                "xmlns:dcterms=\"http://purl.org/dc/terms/\" "
                "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n",
                SCHEMA_PACKAGE);
    VSIFPrintfL(fp, "<cp:revision>0</cp:revision>\n");
    VSIFPrintfL(fp, "</cp:coreProperties>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                            WriteWorkbook()                           */
/************************************************************************/

static bool WriteWorkbook(const char *pszName, GDALDataset *poDS)
{
    CPLString osTmpFilename(CPLSPrintf("/vsizip/%s/xl/workbook.xml", pszName));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp, "<workbook %s xmlns:r=\"%s\">\n", MAIN_NS, SCHEMA_OD_RS);
    VSIFPrintfL(fp, "<fileVersion appName=\"Calc\"/>\n");
    /*
    VSIFPrintfL(fp, "<workbookPr backupFile=\"false\" showObjects=\"all\"
    date1904=\"false\"/>\n"); VSIFPrintfL(fp, "<workbookProtection/>\n");
    VSIFPrintfL(fp, "<bookViews>\n");
    VSIFPrintfL(fp, "<workbookView activeTab=\"0\" firstSheet=\"0\"
    showHorizontalScroll=\"true\" " "showSheetTabs=\"true\"
    showVerticalScroll=\"true\" tabRatio=\"600\" windowHeight=\"8192\" "
                    "windowWidth=\"16384\" xWindow=\"0\" yWindow=\"0\"/>\n");
    VSIFPrintfL(fp, "</bookViews>\n");
    */
    VSIFPrintfL(fp, "<sheets>\n");
    for (int i = 0; i < poDS->GetLayerCount(); i++)
    {
        auto poLayer = poDS->GetLayer(i);
        const char *pszLayerName = poLayer->GetName();
        char *pszXML = OGRGetXML_UTF8_EscapedString(pszLayerName);
        VSIFPrintfL(fp,
                    "<sheet name=\"%s\" sheetId=\"%d\" state=\"visible\" "
                    "r:id=\"rId%d\"/>\n",
                    pszXML, i + 1, i + 2);
        CPLFree(pszXML);
    }
    VSIFPrintfL(fp, "</sheets>\n");
    VSIFPrintfL(fp, "<calcPr iterateCount=\"100\" refMode=\"A1\" "
                    "iterate=\"false\" iterateDelta=\"0.001\"/>\n");
    VSIFPrintfL(fp, "</workbook>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                            BuildColString()                          */
/************************************************************************/

static CPLString BuildColString(int nCol)
{
    /*
    A Z   AA AZ   BA BZ   ZA   ZZ   AAA    ZZZ      AAAA
    0 25  26 51   52 77   676  701  702    18277    18278
    */
    CPLString osRet;
    osRet += (nCol % 26) + 'A';
    while (nCol >= 26)
    {
        nCol /= 26;
        // We would not need a decrement if this was a proper base 26
        // numeration scheme.
        nCol--;
        osRet += (nCol % 26) + 'A';
    }
    const size_t nSize = osRet.size();
    for (size_t l = 0; l < nSize / 2; l++)
    {
        char chTmp = osRet[nSize - 1 - l];
        osRet[nSize - 1 - l] = osRet[l];
        osRet[l] = chTmp;
    }
    return osRet;
}

/************************************************************************/
/*                             WriteLayer()                             */
/************************************************************************/

static bool WriteLayer(const char *pszName, OGRXLSXLayer *poLayer, int iLayer,
                       std::map<std::string, int> &oStringMap,
                       std::vector<std::string> &oStringList)
{
    CPLString osTmpFilename(CPLSPrintf("/vsizip/%s/xl/worksheets/sheet%d.xml",
                                       pszName, iLayer + 1));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp, "<worksheet %s xmlns:r=\"%s\">\n", MAIN_NS, SCHEMA_OD_RS);
    /*
    VSIFPrintfL(fp, "<sheetViews>\n");
    VSIFPrintfL(fp, "<sheetView colorId=\"64\" defaultGridColor=\"true\"
    rightToLeft=\"false\" showFormulas=\"false\" showGridLines=\"true\"
    showOutlineSymbols=\"true\" showRowColHeaders=\"true\" showZeros=\"true\"
    tabSelected=\"%s\" topLeftCell=\"A1\" view=\"normal\"
    windowProtection=\"false\" workbookViewId=\"0\" zoomScale=\"100\"
    zoomScaleNormal=\"100\" zoomScalePageLayoutView=\"60\">\n", (i == 0) ?
    "true" : "false"); VSIFPrintfL(fp, "<selection activeCell=\"A1\"
    activeCellId=\"0\" pane=\"topLeft\" sqref=\"A1\"/>\n"); VSIFPrintfL(fp,
    "</sheetView>\n"); VSIFPrintfL(fp, "</sheetViews>\n");*/

    poLayer->ResetReading();

    OGRFeature *poFeature = poLayer->GetNextFeature();

    OGRFeatureDefn *poFDefn = poLayer->GetLayerDefn();
    bool bHasHeaders = false;
    int iRow = 1;

    const int nFields = poFDefn->GetFieldCount();
    if (nFields > 0)
    {
        VSIFPrintfL(fp, "<cols>\n");
        for (int j = 0; j < nFields; j++)
        {
            int nWidth = 15;
            if (poFDefn->GetFieldDefn(j)->GetType() == OFTDateTime)
                nWidth = 29;
            VSIFPrintfL(fp, "<col min=\"%d\" max=\"%d\" width=\"%d\"/>\n",
                        j + 1, 1024, nWidth);

            if (strcmp(poFDefn->GetFieldDefn(j)->GetNameRef(),
                       CPLSPrintf("Field%d", j + 1)) != 0)
                bHasHeaders = true;
        }
        VSIFPrintfL(fp, "</cols>\n");
    }
    else if (!poLayer->GetCols().empty())
        VSIFPrintfL(fp, "%s\n", poLayer->GetCols().c_str());

    VSIFPrintfL(fp, "<sheetData>\n");

    if (bHasHeaders && poFeature != nullptr)
    {
        VSIFPrintfL(fp, "<row r=\"%d\">\n", iRow);
        for (int j = 0; j < nFields; j++)
        {
            const char *pszVal = poFDefn->GetFieldDefn(j)->GetNameRef();
            std::map<std::string, int>::iterator oIter =
                oStringMap.find(pszVal);
            int nStringIndex = 0;
            if (oIter != oStringMap.end())
                nStringIndex = oIter->second;
            else
            {
                nStringIndex = (int)oStringList.size();
                oStringMap[pszVal] = nStringIndex;
                oStringList.push_back(pszVal);
            }

            CPLString osCol = BuildColString(j);

            VSIFPrintfL(fp, "<c r=\"%s%d\" t=\"s\">\n", osCol.c_str(), iRow);
            VSIFPrintfL(fp, "<v>%d</v>\n", nStringIndex);
            VSIFPrintfL(fp, "</c>\n");
        }
        VSIFPrintfL(fp, "</row>\n");

        iRow++;
    }

    while (poFeature != nullptr)
    {
        VSIFPrintfL(fp, "<row r=\"%d\">\n", iRow);
        for (int j = 0; j < nFields; j++)
        {
            if (poFeature->IsFieldSetAndNotNull(j))
            {
                CPLString osCol = BuildColString(j);

                OGRFieldDefn *poFieldDefn = poFDefn->GetFieldDefn(j);
                OGRFieldType eType = poFieldDefn->GetType();

                if (eType == OFTReal)
                {
                    VSIFPrintfL(fp, "<c r=\"%s%d\">\n", osCol.c_str(), iRow);
                    VSIFPrintfL(fp, "<v>%.16g</v>\n",
                                poFeature->GetFieldAsDouble(j));
                    VSIFPrintfL(fp, "</c>\n");
                }
                else if (eType == OFTInteger)
                {
                    OGRFieldSubType eSubType = poFieldDefn->GetSubType();
                    if (eSubType == OFSTBoolean)
                        VSIFPrintfL(fp, "<c r=\"%s%d\" t=\"b\" s=\"5\">\n",
                                    osCol.c_str(), iRow);
                    else
                        VSIFPrintfL(fp, "<c r=\"%s%d\">\n", osCol.c_str(),
                                    iRow);
                    VSIFPrintfL(fp, "<v>%d</v>\n",
                                poFeature->GetFieldAsInteger(j));
                    VSIFPrintfL(fp, "</c>\n");
                }
                else if (eType == OFTInteger64)
                {
                    VSIFPrintfL(fp, "<c r=\"%s%d\">\n", osCol.c_str(), iRow);
                    VSIFPrintfL(fp, "<v>" CPL_FRMT_GIB "</v>\n",
                                poFeature->GetFieldAsInteger64(j));
                    VSIFPrintfL(fp, "</c>\n");
                }
                else if (eType == OFTDate || eType == OFTDateTime ||
                         eType == OFTTime)
                {
                    int nYear = 0;
                    int nMonth = 0;
                    int nDay = 0;
                    int nHour = 0;
                    int nMinute = 0;
                    int nTZFlag = 0;
                    float fSecond = 0.0f;
                    poFeature->GetFieldAsDateTime(j, &nYear, &nMonth, &nDay,
                                                  &nHour, &nMinute, &fSecond,
                                                  &nTZFlag);
                    struct tm brokendowntime;
                    memset(&brokendowntime, 0, sizeof(brokendowntime));
                    brokendowntime.tm_year =
                        (eType == OFTTime) ? 70 : nYear - 1900;
                    brokendowntime.tm_mon = (eType == OFTTime) ? 0 : nMonth - 1;
                    brokendowntime.tm_mday = (eType == OFTTime) ? 1 : nDay;
                    brokendowntime.tm_hour = nHour;
                    brokendowntime.tm_min = nMinute;
                    brokendowntime.tm_sec = (int)fSecond;
                    GIntBig nUnixTime = CPLYMDHMSToUnixTime(&brokendowntime);
                    double dfNumberOfDaysSince1900 =
                        (1.0 * nUnixTime / NUMBER_OF_SECONDS_PER_DAY);
                    dfNumberOfDaysSince1900 +=
                        fmod(fSecond, 1) / NUMBER_OF_SECONDS_PER_DAY;
                    int s = (eType == OFTDate)       ? 1
                            : (eType == OFTDateTime) ? 2
                                                     : 3;
                    if (eType == OFTDateTime && OGR_GET_MS(fSecond))
                        s = 4;
                    VSIFPrintfL(fp, "<c r=\"%s%d\" s=\"%d\">\n", osCol.c_str(),
                                iRow, s);
                    if (eType != OFTTime)
                        dfNumberOfDaysSince1900 +=
                            NUMBER_OF_DAYS_BETWEEN_1900_AND_1970;
                    if (eType == OFTDate)
                        VSIFPrintfL(fp, "<v>%d</v>\n",
                                    (int)(dfNumberOfDaysSince1900 + 0.1));
                    else
                        VSIFPrintfL(fp, "<v>%.16g</v>\n",
                                    dfNumberOfDaysSince1900);
                    VSIFPrintfL(fp, "</c>\n");
                }
                else
                {
                    const char *pszVal = poFeature->GetFieldAsString(j);
                    std::map<std::string, int>::iterator oIter =
                        oStringMap.find(pszVal);
                    int nStringIndex = 0;
                    if (oIter != oStringMap.end())
                        nStringIndex = oIter->second;
                    else
                    {
                        nStringIndex = (int)oStringList.size();
                        oStringMap[pszVal] = nStringIndex;
                        oStringList.push_back(pszVal);
                    }
                    VSIFPrintfL(fp, "<c r=\"%s%d\" t=\"s\">\n", osCol.c_str(),
                                iRow);
                    VSIFPrintfL(fp, "<v>%d</v>\n", nStringIndex);
                    VSIFPrintfL(fp, "</c>\n");
                }
            }
        }
        VSIFPrintfL(fp, "</row>\n");

        iRow++;
        delete poFeature;
        poFeature = poLayer->GetNextFeature();
    }
    VSIFPrintfL(fp, "</sheetData>\n");
    VSIFPrintfL(fp, "</worksheet>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                        WriteSharedStrings()                          */
/************************************************************************/

static bool WriteSharedStrings(const char *pszName,
                               std::vector<std::string> &oStringList)
{
    CPLString osTmpFilename(
        CPLSPrintf("/vsizip/%s/xl/sharedStrings.xml", pszName));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp, "<sst %s uniqueCount=\"%d\">\n", MAIN_NS,
                (int)oStringList.size());
    for (int i = 0; i < (int)oStringList.size(); i++)
    {
        VSIFPrintfL(fp, "<si>\n");
        char *pszXML = OGRGetXML_UTF8_EscapedString(oStringList[i].c_str());
        VSIFPrintfL(fp, "<t>%s</t>\n", pszXML);
        CPLFree(pszXML);
        VSIFPrintfL(fp, "</si>\n");
    }
    VSIFPrintfL(fp, "</sst>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                           WriteStyles()                              */
/************************************************************************/

static bool WriteStyles(const char *pszName)
{
    CPLString osTmpFilename(CPLSPrintf("/vsizip/%s/xl/styles.xml", pszName));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp, "<styleSheet %s>\n", MAIN_NS);
    VSIFPrintfL(fp, "<numFmts count=\"4\">\n");
    VSIFPrintfL(fp, "<numFmt formatCode=\"GENERAL\" numFmtId=\"164\"/>\n");
    VSIFPrintfL(fp, "<numFmt formatCode=\"DD/MM/YY\" numFmtId=\"165\"/>\n");
    VSIFPrintfL(
        fp,
        "<numFmt formatCode=\"DD/MM/YYYY\\ HH:MM:SS\" numFmtId=\"166\"/>\n");
    VSIFPrintfL(fp, "<numFmt formatCode=\"HH:MM:SS\" numFmtId=\"167\"/>\n");
    VSIFPrintfL(fp, "<numFmt formatCode=\"DD/MM/YYYY\\ HH:MM:SS.000\" "
                    "numFmtId=\"168\"/>\n");
    VSIFPrintfL(fp, "<numFmt "
                    "formatCode=\"&quot;TRUE&quot;;&quot;TRUE&quot;;&quot;"
                    "FALSE&quot;\" numFmtId=\"169\"/>\n");
    VSIFPrintfL(fp, "</numFmts>\n");
    VSIFPrintfL(fp, "<fonts count=\"1\">\n");
    VSIFPrintfL(fp, "<font>\n");
    VSIFPrintfL(fp, "<name val=\"Arial\"/>\n");
    VSIFPrintfL(fp, "<family val=\"2\"/>\n");
    VSIFPrintfL(fp, "<sz val=\"10\"/>\n");
    VSIFPrintfL(fp, "</font>\n");
    VSIFPrintfL(fp, "</fonts>\n");
    VSIFPrintfL(fp, "<fills count=\"1\">\n");
    VSIFPrintfL(fp, "<fill>\n");
    VSIFPrintfL(fp, "<patternFill patternType=\"none\"/>\n");
    VSIFPrintfL(fp, "</fill>\n");
    VSIFPrintfL(fp, "</fills>\n");
    VSIFPrintfL(fp, "<borders count=\"1\">\n");
    VSIFPrintfL(fp, "<border diagonalDown=\"false\" diagonalUp=\"false\">\n");
    VSIFPrintfL(fp, "<left/>\n");
    VSIFPrintfL(fp, "<right/>\n");
    VSIFPrintfL(fp, "<top/>\n");
    VSIFPrintfL(fp, "<bottom/>\n");
    VSIFPrintfL(fp, "<diagonal/>\n");
    VSIFPrintfL(fp, "</border>\n");
    VSIFPrintfL(fp, "</borders>\n");
    VSIFPrintfL(fp, "<cellStyleXfs count=\"1\">\n");
    VSIFPrintfL(fp, "<xf numFmtId=\"164\">\n");
    VSIFPrintfL(fp, "</xf>\n");
    VSIFPrintfL(fp, "</cellStyleXfs>\n");
    VSIFPrintfL(fp, "<cellXfs count=\"6\">\n");
    VSIFPrintfL(fp, "<xf numFmtId=\"164\" xfId=\"0\"/>\n");
    VSIFPrintfL(fp, "<xf numFmtId=\"165\" xfId=\"0\"/>\n");
    VSIFPrintfL(fp, "<xf numFmtId=\"166\" xfId=\"0\"/>\n");
    VSIFPrintfL(fp, "<xf numFmtId=\"167\" xfId=\"0\"/>\n");
    VSIFPrintfL(fp, "<xf numFmtId=\"168\" xfId=\"0\"/>\n");
    VSIFPrintfL(fp, "<xf numFmtId=\"169\" xfId=\"0\"/>\n");
    VSIFPrintfL(fp, "</cellXfs>\n");
    VSIFPrintfL(fp, "<cellStyles count=\"1\">\n");
    VSIFPrintfL(fp, "<cellStyle builtinId=\"0\" customBuiltin=\"false\" "
                    "name=\"Normal\" xfId=\"0\"/>\n");
    VSIFPrintfL(fp, "</cellStyles>\n");
    VSIFPrintfL(fp, "</styleSheet>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                           WriteWorkbookRels()                        */
/************************************************************************/

static bool WriteWorkbookRels(const char *pszName, int nLayers)
{
    CPLString osTmpFilename(
        CPLSPrintf("/vsizip/%s/xl/_rels/workbook.xml.rels", pszName));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp, "<Relationships xmlns=\"%s\">\n", SCHEMA_PACKAGE_RS);
    VSIFPrintfL(fp,
                "<Relationship Id=\"rId1\" Type=\"%s/styles\" "
                "Target=\"styles.xml\"/>\n",
                SCHEMA_OD_RS);
    for (int i = 0; i < nLayers; i++)
    {
        VSIFPrintfL(fp,
                    "<Relationship Id=\"rId%d\" Type=\"%s/worksheet\" "
                    "Target=\"worksheets/sheet%d.xml\"/>\n",
                    2 + i, SCHEMA_OD_RS, 1 + i);
    }
    VSIFPrintfL(fp,
                "<Relationship Id=\"rId%d\" Type=\"%s/sharedStrings\" "
                "Target=\"sharedStrings.xml\"/>\n",
                2 + nLayers, SCHEMA_OD_RS);
    VSIFPrintfL(fp, "</Relationships>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                             WriteDotRels()                           */
/************************************************************************/

static bool WriteDotRels(const char *pszName)
{
    CPLString osTmpFilename(CPLSPrintf("/vsizip/%s/_rels/.rels", pszName));
    VSILFILE *fp = VSIFOpenL(osTmpFilename, "wb");
    if (!fp)
        return false;
    VSIFWriteL(XML_HEADER, strlen(XML_HEADER), 1, fp);
    VSIFPrintfL(fp, "<Relationships xmlns=\"%s\">\n", SCHEMA_PACKAGE_RS);
    VSIFPrintfL(fp,
                "<Relationship Id=\"rId1\" Type=\"%s/officeDocument\" "
                "Target=\"xl/workbook.xml\"/>\n",
                SCHEMA_OD_RS);
    VSIFPrintfL(
        fp,
        "<Relationship Id=\"rId2\" Type=\"%s/metadata/core-properties\" "
        "Target=\"docProps/core.xml\"/>\n",
        SCHEMA_PACKAGE_RS);
    VSIFPrintfL(fp,
                "<Relationship Id=\"rId3\" Type=\"%s/extended-properties\" "
                "Target=\"docProps/app.xml\"/>\n",
                SCHEMA_OD_RS);
    VSIFPrintfL(fp, "</Relationships>\n");
    VSIFCloseL(fp);
    return true;
}

/************************************************************************/
/*                            FlushCache()                              */
/************************************************************************/

CPLErr OGRXLSXDataSource::FlushCache(bool /* bAtClosing */)
{
    if (!bUpdated)
        return CE_None;

    /* Cause all layers to be loaded */
    for (int i = 0; i < nLayers; i++)
    {
        ((OGRXLSXLayer *)papoLayers[i])->GetLayerDefn();
    }

    VSIStatBufL sStat;
    if (VSIStatL(pszName, &sStat) == 0)
    {
        if (VSIUnlink(pszName) != 0)
        {
            CPLError(CE_Failure, CPLE_FileIO, "Cannot delete %s", pszName);
            return CE_Failure;
        }
    }

    CPLConfigOptionSetter oZip64Disable("CPL_CREATE_ZIP64", "NO", false);

    /* Maintain new ZIP files opened */
    CPLString osTmpFilename(CPLSPrintf("/vsizip/%s", pszName));
    VSILFILE *fpZIP = VSIFOpenExL(osTmpFilename, "wb", true);
    if (fpZIP == nullptr)
    {
        CPLError(CE_Failure, CPLE_FileIO, "Cannot create %s: %s", pszName,
                 VSIGetLastErrorMsg());
        return CE_Failure;
    }

    bool bOK = WriteContentTypes(pszName, nLayers);

    // VSIMkdir(CPLSPrintf("/vsizip/%s/docProps", pszName),0755);
    bOK &= WriteApp(pszName);
    bOK &= WriteCore(pszName);

    // VSIMkdir(CPLSPrintf("/vsizip/%s/xl", pszName),0755);
    bOK &= WriteWorkbook(pszName, this);

    std::map<std::string, int> oStringMap;
    std::vector<std::string> oStringList;

    // VSIMkdir(CPLSPrintf("/vsizip/%s/xl/worksheets", pszName),0755);
    for (int i = 0; i < nLayers; i++)
    {
        bOK &= WriteLayer(pszName, papoLayers[i], i, oStringMap, oStringList);
    }

    bOK &= WriteSharedStrings(pszName, oStringList);
    bOK &= WriteStyles(pszName);

    // VSIMkdir(CPLSPrintf("/vsizip/%s/xl/_rels", pszName),0755);
    bOK &= WriteWorkbookRels(pszName, nLayers);

    // VSIMkdir(CPLSPrintf("/vsizip/%s/_rels", pszName),0755);
    bOK &= WriteDotRels(pszName);

    /* Now close ZIP file */
    if (VSIFCloseL(fpZIP) != 0)
        bOK = false;

    /* Reset updated flag at datasource and layer level */
    bUpdated = false;
    for (int i = 0; i < nLayers; i++)
    {
        ((OGRXLSXLayer *)papoLayers[i])->SetUpdated(false);
    }

    if (!bOK)
    {
        CPLError(CE_Failure, CPLE_FileIO, "Failure when saving %s", pszName);
    }

    return bOK ? CE_None : CE_Failure;
}

}  // namespace OGRXLSX
