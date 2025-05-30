/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  SQLite Virtual Table module using OGR layers
 * Author:   Even Rouault, even dot rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2012-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#define DEFINE_OGRSQLiteSQLFunctionsSetCaseSensitiveLike

#include "cpl_port.h"
#include "ogrsqlitevirtualogr.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"

/************************************************************************/
/*                  OGR2SQLITE_GetNameForGeometryColumn()               */
/************************************************************************/

CPLString OGR2SQLITE_GetNameForGeometryColumn(OGRLayer *poLayer)
{
    const char *pszGeomColumn = poLayer->GetGeometryColumn();
    if (pszGeomColumn != nullptr && !EQUAL(pszGeomColumn, ""))
    {
        if (poLayer->GetLayerDefn()->GetFieldIndex(pszGeomColumn) < 0)
            return pszGeomColumn;
    }

    CPLString osGeomCol("GEOMETRY");
    int nTry = 2;
    while (poLayer->GetLayerDefn()->GetFieldIndex(osGeomCol) >= 0)
    {
        osGeomCol.Printf("GEOMETRY%d", nTry++);
    }
    return osGeomCol;
}

#if !defined(HAVE_SQLITE3EXT_H)

// Stub functions

void OGR2SQLITE_Register()
{
}

OGR2SQLITEModule *OGR2SQLITE_Setup(GDALDataset *, OGRSQLiteDataSource *)
{
    return nullptr;
}

void OGR2SQLITE_SetCaseSensitiveLike(OGR2SQLITEModule *, bool)
{
}

int OGR2SQLITE_AddExtraDS(OGR2SQLITEModule *, GDALDataset *)
{
    return 0;
}

#else

/************************************************************************/
/*                           OGR2SQLITE_Register()                      */
/************************************************************************/

CPL_C_START
int CPL_DLL OGR2SQLITE_static_register(sqlite3 *hDB, char **pzErrMsg,
                                       void *pApi);
CPL_C_END

/* We call this function so that each time a db is created, */
/* OGR2SQLITE_static_register is called, to initialize the sqlite3_api */
/* structure with the right pointers. */
/* We need to declare this function before including sqlite3ext.h, since */
/* sqlite 3.8.7, sqlite3_auto_extension can be a macro (#5725) */

void OGR2SQLITE_Register()
{
#if !defined(SQLITE_HAS_NON_DEPRECATED_AUTO_EXTENSION) && defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated"
#endif
    sqlite3_auto_extension(
        reinterpret_cast<void (*)(void)>(OGR2SQLITE_static_register));
#if !defined(SQLITE_HAS_NON_DEPRECATED_AUTO_EXTENSION) && defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}

#define VIRTUAL_OGR_DYNAMIC_EXTENSION_ENABLED
// #define DEBUG_OGR2SQLITE

#include "gdal_priv.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogr_p.h"
#include "ogr_spatialref.h"
#include "ogrsf_frmts.h"
#include "ogrsqlitesqlfunctions.h"
#include "ogrsqliteutility.h"
#include "ogr_swq.h"
#include "sqlite3.h"
#include "sqlite3ext.h"

#undef SQLITE_EXTENSION_INIT1
#define SQLITE_EXTENSION_INIT1                                                 \
    const sqlite3_api_routines *sqlite3_api = nullptr;

/* Declaration of sqlite3_api structure */
static SQLITE_EXTENSION_INIT1

/* The layout of fields is :
   0   : RegularField0
   ...
   n-1 : RegularField(n-1)
   n   : OGR_STYLE (may be HIDDEN)
   n+1 : GEOMETRY
*/

#define COMPILATION_ALLOWED
#include "ogrsqlitesqlfunctions.cpp" /* yes the .cpp file, to make it work on Windows with load_extension('gdalXX.dll') */
#undef COMPILATION_ALLOWED

    /************************************************************************/
    /*                           OGR2SQLITEModule                           */
    /************************************************************************/

    class OGR2SQLITEModule
{
#ifdef DEBUG
    void *pDummy = nullptr; /* to track memory leaks */
#endif
    sqlite3 *hDB = nullptr; /* *NOT* to be freed */

    GDALDataset *poDS = nullptr; /* *NOT* to be freed */
    std::vector<std::unique_ptr<GDALDataset>>
        apoExtraDS{}; /* each datasource to be freed */

    OGRSQLiteDataSource *poSQLiteDS =
        nullptr; /* *NOT* to be freed, might be NULL */

    std::map<CPLString, OGRLayer *> oMapVTableToOGRLayer{};

    void *hHandleSQLFunctions = nullptr;

    CPL_DISALLOW_COPY_ASSIGN(OGR2SQLITEModule)

  public:
    OGR2SQLITEModule();
    ~OGR2SQLITEModule();

    int Setup(GDALDataset *poDS, OGRSQLiteDataSource *poSQLiteDS);
    int Setup(sqlite3 *hDB);

    GDALDataset *GetDS()
    {
        return poDS;
    }

    int AddExtraDS(GDALDataset *poDS);
    GDALDataset *GetExtraDS(int nIndex);

    int FetchSRSId(const OGRSpatialReference *poSRS);

    void RegisterVTable(const char *pszVTableName, OGRLayer *poLayer);
    void UnregisterVTable(const char *pszVTableName);
    OGRLayer *GetLayerForVTable(const char *pszVTableName);

    void SetHandleSQLFunctions(void *hHandleSQLFunctionsIn);

    void SetCaseSensitiveLike(bool b)
    {
        OGRSQLiteSQLFunctionsSetCaseSensitiveLike(hHandleSQLFunctions, b);
    }
};

/************************************************************************/
/*                        OGR2SQLITEModule()                            */
/************************************************************************/

OGR2SQLITEModule::OGR2SQLITEModule()
#ifdef DEBUG
    : pDummy(CPLMalloc(1))
#endif
{
}

/************************************************************************/
/*                          ~OGR2SQLITEModule                           */
/************************************************************************/

OGR2SQLITEModule::~OGR2SQLITEModule()
{
#ifdef DEBUG
    CPLFree(pDummy);
#endif

    apoExtraDS.clear();

    OGRSQLiteUnregisterSQLFunctions(hHandleSQLFunctions);
}

/************************************************************************/
/*                        SetHandleSQLFunctions()                       */
/************************************************************************/

void OGR2SQLITEModule::SetHandleSQLFunctions(void *hHandleSQLFunctionsIn)
{
    CPLAssert(hHandleSQLFunctions == nullptr);
    hHandleSQLFunctions = hHandleSQLFunctionsIn;
}

/************************************************************************/
/*                            AddExtraDS()                              */
/************************************************************************/

int OGR2SQLITEModule::AddExtraDS(GDALDataset *poDSIn)
{
    const int nRet = static_cast<int>(apoExtraDS.size());
    apoExtraDS.push_back(std::unique_ptr<GDALDataset>(poDSIn));
    return nRet;
}

/************************************************************************/
/*                            GetExtraDS()                              */
/************************************************************************/

GDALDataset *OGR2SQLITEModule::GetExtraDS(int nIndex)
{
    if (nIndex < 0 || nIndex >= static_cast<int>(apoExtraDS.size()))
        return nullptr;
    return apoExtraDS[nIndex].get();
}

/************************************************************************/
/*                                Setup()                               */
/************************************************************************/

int OGR2SQLITEModule::Setup(GDALDataset *poDSIn,
                            OGRSQLiteDataSource *poSQLiteDSIn)
{
    CPLAssert(poDS == nullptr);
    CPLAssert(poSQLiteDS == nullptr);
    poDS = poDSIn;
    poSQLiteDS = poSQLiteDSIn;
    return Setup(poSQLiteDS->GetDB());
}

/************************************************************************/
/*                            FetchSRSId()                              */
/************************************************************************/

// TODO(schwehr): Refactor FetchSRSId to be much simpler.
int OGR2SQLITEModule::FetchSRSId(const OGRSpatialReference *poSRS)
{
    int nSRSId = -1;

    if (poSQLiteDS != nullptr)
    {
        nSRSId = poSQLiteDS->GetUndefinedSRID();
        if (poSRS != nullptr)
            nSRSId = poSQLiteDS->FetchSRSId(poSRS);
    }
    else
    {
        if (poSRS != nullptr)
        {
            const char *pszAuthorityName = poSRS->GetAuthorityName(nullptr);
            if (pszAuthorityName != nullptr && EQUAL(pszAuthorityName, "EPSG"))
            {
                const char *pszAuthorityCode = poSRS->GetAuthorityCode(nullptr);
                if (pszAuthorityCode != nullptr && strlen(pszAuthorityCode) > 0)
                {
                    nSRSId = atoi(pszAuthorityCode);
                }
            }
        }
    }

    return nSRSId;
}

/************************************************************************/
/*                          RegisterVTable()                            */
/************************************************************************/

void OGR2SQLITEModule::RegisterVTable(const char *pszVTableName,
                                      OGRLayer *poLayer)
{
    oMapVTableToOGRLayer[pszVTableName] = poLayer;
}

/************************************************************************/
/*                          UnregisterVTable()                          */
/************************************************************************/

void OGR2SQLITEModule::UnregisterVTable(const char *pszVTableName)
{
    oMapVTableToOGRLayer[pszVTableName] = nullptr;
}

/************************************************************************/
/*                          GetLayerForVTable()                         */
/************************************************************************/

OGRLayer *OGR2SQLITEModule::GetLayerForVTable(const char *pszVTableName)
{
    std::map<CPLString, OGRLayer *>::iterator oIter =
        oMapVTableToOGRLayer.find(pszVTableName);
    if (oIter == oMapVTableToOGRLayer.end())
    {
        if (poDS == poSQLiteDS)
            return poSQLiteDS->GetLayerByName(pszVTableName);
        return nullptr;
    }

    OGRLayer *poLayer = oIter->second;
    if (poLayer == nullptr)
    {
        /* If the associate layer is null, then try to "ping" the virtual */
        /* table since we know that we have managed to create it before */
        if (sqlite3_exec(hDB,
                         CPLSPrintf("PRAGMA table_info(\"%s\")",
                                    SQLEscapeName(pszVTableName).c_str()),
                         nullptr, nullptr, nullptr) == SQLITE_OK)
        {
            poLayer = oMapVTableToOGRLayer[pszVTableName];
        }
    }

    return poLayer;
}

/* See http://www.sqlite.org/vtab.html for the documentation on how to
   implement a new module for the Virtual Table mechanism. */

/************************************************************************/
/*                            OGR2SQLITE_vtab                           */
/************************************************************************/

typedef struct
{
    /* Mandatory fields by sqlite3: don't change or reorder them ! */
    const sqlite3_module *pModule;
    int nRef;
    char *zErrMsg;

    /* Extension fields */
    char *pszVTableName;
    OGR2SQLITEModule *poModule;
    GDALDataset *poDS;
    int bCloseDS;
    OGRLayer *poLayer;
    int nMyRef;
    bool bHasFIDColumn;
} OGR2SQLITE_vtab;

/************************************************************************/
/*                          OGR2SQLITE_vtab_cursor                      */
/************************************************************************/

typedef struct
{
    /* Mandatory fields by sqlite3: don't change or reorder them ! */
    OGR2SQLITE_vtab *pVTab;

    /* Extension fields */
    GDALDataset *poDupDataSource;
    OGRLayer *poLayer;
    OGRFeature *poFeature;

    /* nFeatureCount >= 0 if the layer has a feast feature count capability. */
    /* In which case nNextWishedIndex and nCurFeatureIndex */
    /* will be used to avoid useless GetNextFeature() */
    /* Helps in SELECT COUNT(*) FROM xxxx scenarios */
    GIntBig nFeatureCount;
    GIntBig nNextWishedIndex;
    GIntBig nCurFeatureIndex;

    GByte *pabyGeomBLOB;
    int nGeomBLOBLen;
} OGR2SQLITE_vtab_cursor;

#ifdef VIRTUAL_OGR_DYNAMIC_EXTENSION_ENABLED

/************************************************************************/
/*                     OGR2SQLITEDetectSuspiciousUsage()                */
/************************************************************************/

static int OGR2SQLITEDetectSuspiciousUsage(sqlite3 *hDB,
                                           const char *pszVirtualTableName,
                                           char **pzErr)
{
    char **papszResult = nullptr;
    int nRowCount = 0;
    int nColCount = 0;

    /* Collect database names */
    sqlite3_get_table(hDB, "PRAGMA database_list", &papszResult, &nRowCount,
                      &nColCount, nullptr);

    std::vector<CPLString> aosDatabaseNames;
    for (int i = 1; i <= nRowCount; i++)
    {
        const char *pszUnescapedName = papszResult[i * nColCount + 1];
        aosDatabaseNames.push_back(CPLSPrintf(
            "\"%s\".sqlite_master", SQLEscapeName(pszUnescapedName).c_str()));
    }

    /* Add special database (just in case, not sure it is really needed) */
    aosDatabaseNames.push_back("sqlite_temp_master");

    sqlite3_free_table(papszResult);
    papszResult = nullptr;

    /* Check the triggers of each database */
    for (const auto &osDBName : aosDatabaseNames)
    {
        nRowCount = 0;
        nColCount = 0;

        const char *pszSQL =
            CPLSPrintf("SELECT name, sql FROM %s "
                       "WHERE (type = 'trigger' OR type = 'view') AND ("
                       "sql LIKE '%%%s%%' OR "
                       "sql LIKE '%%\"%s\"%%' OR "
                       "sql LIKE '%%ogr_layer_%%' )",
                       osDBName.c_str(), pszVirtualTableName,
                       SQLEscapeName(pszVirtualTableName).c_str());

        sqlite3_get_table(hDB, pszSQL, &papszResult, &nRowCount, &nColCount,
                          nullptr);

        sqlite3_free_table(papszResult);
        papszResult = nullptr;

        if (nRowCount > 0)
        {
            if (!CPLTestBool(CPLGetConfigOption(
                    "ALLOW_VIRTUAL_OGR_FROM_TRIGGER_AND_VIEW", "NO")))
            {
                *pzErr = sqlite3_mprintf(
                    "A trigger and/or view might reference VirtualOGR table "
                    "'%s'.\n"
                    "This is suspicious practice that could be used to steal "
                    "data without your consent.\n"
                    "Disabling access to it unless you define the "
                    "ALLOW_VIRTUAL_OGR_FROM_TRIGGER_AND_VIEW "
                    "configuration option to YES.",
                    pszVirtualTableName);
                return TRUE;
            }
        }
    }

    return FALSE;
}

#endif  // VIRTUAL_OGR_DYNAMIC_EXTENSION_ENABLED

/************************************************************************/
/*                      OGR2SQLITE_ConnectCreate()                      */
/************************************************************************/

static int OGR2SQLITE_DisconnectDestroy(sqlite3_vtab *pVTab);

static int OGR2SQLITE_ConnectCreate(sqlite3 *hDB, void *pAux, int argc,
                                    const char *const *argv,
                                    sqlite3_vtab **ppVTab, char **pzErr)
{
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "ConnectCreate(%s)", argv[2]);
#endif

    OGR2SQLITEModule *poModule = static_cast<OGR2SQLITEModule *>(pAux);
    OGRLayer *poLayer = nullptr;
    bool bExposeOGR_STYLE = false;
    bool bCloseDS = false;
    bool bInternalUse = false;
    bool bExposeOGRNativeData = false;

    /* -------------------------------------------------------------------- */
    /*      If called from ogrexecutesql.cpp                                */
    /* -------------------------------------------------------------------- */
    GDALDataset *poDS = poModule->GetDS();
    if (poDS != nullptr && (argc == 6 || argc == 7) &&
        CPLGetValueType(argv[3]) == CPL_VALUE_INTEGER)
    {
        bInternalUse = true;

        int nDSIndex = atoi(argv[3]);
        if (nDSIndex >= 0)
        {
            poDS = poModule->GetExtraDS(nDSIndex);
            if (poDS == nullptr)
            {
                *pzErr =
                    sqlite3_mprintf("Invalid dataset index : %d", nDSIndex);
                return SQLITE_ERROR;
            }
        }
        CPLString osLayerName(SQLUnescape(argv[4]));

        poLayer = poDS->GetLayerByName(osLayerName);
        if (poLayer == nullptr)
        {
            *pzErr =
                sqlite3_mprintf("Cannot find layer '%s' in '%s'",
                                osLayerName.c_str(), poDS->GetDescription());
            return SQLITE_ERROR;
        }

        bExposeOGR_STYLE = atoi(SQLUnescape(argv[5])) != 0;
        bExposeOGRNativeData =
            (argc == 7) ? atoi(SQLUnescape(argv[6])) != 0 : false;
    }
#ifdef VIRTUAL_OGR_DYNAMIC_EXTENSION_ENABLED
    /* -------------------------------------------------------------------- */
    /*      If called from outside (OGR loaded as a sqlite3 extension)      */
    /* -------------------------------------------------------------------- */
    else
    {
        if (argc < 4 || argc > 8)
        {
            *pzErr = sqlite3_mprintf(
                "Expected syntax: CREATE VIRTUAL TABLE xxx USING "
                "VirtualOGR(datasource_name[, update_mode, [layer_name[, "
                "expose_ogr_style[, expose_ogr_native_data]]]])");
            return SQLITE_ERROR;
        }

        if (OGR2SQLITEDetectSuspiciousUsage(hDB, argv[2], pzErr))
        {
            return SQLITE_ERROR;
        }

        CPLString osDSName(SQLUnescape(argv[3]));
        CPLString osUpdate(SQLUnescape((argc >= 5) ? argv[4] : "0"));

        if (!EQUAL(osUpdate, "1") && !EQUAL(osUpdate, "0"))
        {
            *pzErr = sqlite3_mprintf("update_mode parameter should be 0 or 1");
            return SQLITE_ERROR;
        }

        const bool bUpdate = atoi(osUpdate) != 0;

        poDS = GDALDataset::Open(
            osDSName.c_str(),
            GDAL_OF_VECTOR | (bUpdate ? GDAL_OF_UPDATE : GDAL_OF_READONLY),
            nullptr, nullptr, nullptr);
        if (poDS == nullptr)
        {
            *pzErr = sqlite3_mprintf("Cannot open datasource '%s'",
                                     osDSName.c_str());
            return SQLITE_ERROR;
        }

        CPLString osLayerName;
        if (argc >= 6)
        {
            osLayerName = SQLUnescape(argv[5]);
            poLayer = poDS->GetLayerByName(osLayerName);
        }
        else
        {
            if (poDS->GetLayerCount() == 0)
            {
                *pzErr = sqlite3_mprintf("Datasource '%s' has no layers",
                                         osDSName.c_str());
                poDS->Release();
                return SQLITE_ERROR;
            }

            if (poDS->GetLayerCount() > 1)
            {
                *pzErr =
                    sqlite3_mprintf("Datasource '%s' has more than one layers, "
                                    "and none was explicitly selected.",
                                    osDSName.c_str());
                poDS->Release();
                return SQLITE_ERROR;
            }

            poLayer = poDS->GetLayer(0);
        }

        if (poLayer == nullptr)
        {
            *pzErr = sqlite3_mprintf("Cannot find layer '%s' in '%s'",
                                     osLayerName.c_str(), osDSName.c_str());
            poDS->Release();
            return SQLITE_ERROR;
        }

        if (argc >= 7)
        {
            bExposeOGR_STYLE = atoi(SQLUnescape(argv[6])) != 0;
        }
        if (argc >= 8)
        {
            bExposeOGRNativeData = atoi(SQLUnescape(argv[7])) != 0;
        }

        bCloseDS = true;
    }
#endif  // VIRTUAL_OGR_DYNAMIC_EXTENSION_ENABLED
    OGR2SQLITE_vtab *vtab =
        static_cast<OGR2SQLITE_vtab *>(CPLCalloc(1, sizeof(OGR2SQLITE_vtab)));
    /* We do not need to fill the non-extended fields */
    vtab->pszVTableName = CPLStrdup(SQLEscapeName(argv[2]));
    vtab->poModule = poModule;
    vtab->poDS = poDS;
    vtab->bCloseDS = bCloseDS;
    vtab->poLayer = poLayer;
    vtab->nMyRef = 0;

    poModule->RegisterVTable(vtab->pszVTableName, poLayer);

    *ppVTab = reinterpret_cast<sqlite3_vtab *>(vtab);

    CPLString osSQL;
    osSQL = "CREATE TABLE ";
    osSQL += "\"";
    osSQL += SQLEscapeName(argv[2]);
    osSQL += "\"";
    osSQL += "(";

    bool bAddComma = false;

    const OGRFeatureDefn *poFDefn = poLayer->GetLayerDefn();

    const char *pszFIDColumn = poLayer->GetFIDColumn();
    std::set<std::string> oSetNamesUC;
    if (pszFIDColumn[0])
    {
        osSQL += "\"";
        osSQL += SQLEscapeName(pszFIDColumn);
        oSetNamesUC.insert(CPLString(pszFIDColumn).toupper());
        if (poFDefn->GetFieldIndex(pszFIDColumn) >= 0)
            osSQL += "\" INTEGER PRIMARY KEY NOT NULL";
        else
            osSQL += "\" INTEGER HIDDEN PRIMARY KEY NOT NULL";
        bAddComma = true;
        vtab->bHasFIDColumn = true;
    }

    bool bHasOGR_STYLEField = false;
    for (int i = 0; i < poFDefn->GetFieldCount(); i++)
    {
        const OGRFieldDefn *poFieldDefn = poFDefn->GetFieldDefn(i);

        if (bAddComma)
            osSQL += ",";
        bAddComma = true;

        if (EQUAL(poFieldDefn->GetNameRef(), "OGR_STYLE"))
            bHasOGR_STYLEField = true;

        CPLString osFieldName(poFieldDefn->GetNameRef());
        int nCounter = 2;
        while (oSetNamesUC.find(CPLString(osFieldName).toupper()) !=
               oSetNamesUC.end())
        {
            do
            {
                osFieldName.Printf("%s%d", poFieldDefn->GetNameRef(), nCounter);
                nCounter++;
            } while (poFDefn->GetFieldIndex(osFieldName) >= 0);
        }
        oSetNamesUC.insert(CPLString(osFieldName).toupper());

        osSQL += "\"";
        osSQL += SQLEscapeName(osFieldName);
        osSQL += "\"";
        osSQL += " ";

        std::string osType(OGRSQLiteFieldDefnToSQliteFieldDefn(
            poFieldDefn, bInternalUse, false));
        if (bInternalUse)
        {
            const auto &osDomainName = poFieldDefn->GetDomainName();
            if (!osDomainName.empty())
            {
                // Avoids illegal VARCHAR(n)_BEGIN_DOMAIN_NAME_... to be
                // emitted
                if (poFieldDefn->GetType() == OFTString)
                    osType = "VARCHAR";
                osType += "_BEGIN_DOMAIN_NAME_";
                char *pszEncoded = CPLBinaryToHex(
                    static_cast<int>(osDomainName.size()),
                    reinterpret_cast<const GByte *>(osDomainName.data()));
                osType += pszEncoded;
                CPLFree(pszEncoded);
                osType += "_END_DOMAIN_NAME";
            }
        }
        osSQL += osType;

        if (EQUAL(poFieldDefn->GetNameRef(), pszFIDColumn))
            osSQL += " HIDDEN";
    }

    if (bAddComma)
        osSQL += ",";

    if (bHasOGR_STYLEField)
    {
        osSQL += "'dummy' VARCHAR HIDDEN";
    }
    else
    {
        osSQL += "OGR_STYLE VARCHAR";
        if (!bExposeOGR_STYLE)
            osSQL += " HIDDEN";
    }

    for (int i = 0; i < poFDefn->GetGeomFieldCount(); i++)
    {
        osSQL += ",";

        const OGRGeomFieldDefn *poFieldDefn = poFDefn->GetGeomFieldDefn(i);

        osSQL += "\"";
        if (i == 0)
            osSQL +=
                SQLEscapeName(OGR2SQLITE_GetNameForGeometryColumn(poLayer));
        else
            osSQL += SQLEscapeName(poFieldDefn->GetNameRef());
        osSQL += "\"";
        osSQL += " BLOB";

        /* We use a special column type, e.g. BLOB_POINT_25D_4326 */
        /* when the virtual table is created by OGRSQLiteExecuteSQL() */
        /* and thus for internal use only. */
        if (bInternalUse)
        {
            osSQL += "_";
            osSQL += OGRToOGCGeomType(poFieldDefn->GetType());
            osSQL += "_XY";
            if (wkbHasZ(poFieldDefn->GetType()))
                osSQL += "Z";
            if (wkbHasM(poFieldDefn->GetType()))
                osSQL += "M";
            const OGRSpatialReference *poSRS = poFieldDefn->GetSpatialRef();
            if (poSRS == nullptr && i == 0)
                poSRS = poLayer->GetSpatialRef();
            int nSRID = poModule->FetchSRSId(poSRS);
            if (nSRID >= 0)
            {
                osSQL += "_";
                osSQL += CPLSPrintf("%d", nSRID);
            }
        }
    }

    osSQL += ", OGR_NATIVE_DATA VARCHAR";
    if (!bExposeOGRNativeData)
        osSQL += " HIDDEN";
    osSQL += ", OGR_NATIVE_MEDIA_TYPE VARCHAR";
    if (!bExposeOGRNativeData)
        osSQL += " HIDDEN";

    osSQL += ")";

    CPLDebug("OGR2SQLITE", "sqlite3_declare_vtab(%s)", osSQL.c_str());
    if (sqlite3_declare_vtab(hDB, osSQL.c_str()) != SQLITE_OK)
    {
        *pzErr = sqlite3_mprintf("CREATE VIRTUAL: invalid SQL statement : %s",
                                 osSQL.c_str());
        OGR2SQLITE_DisconnectDestroy(reinterpret_cast<sqlite3_vtab *>(vtab));
        return SQLITE_ERROR;
    }

    return SQLITE_OK;
}

/************************************************************************/
/*                       OGR2SQLITE_IsHandledOp()                       */
/************************************************************************/

static bool OGR2SQLITE_IsHandledOp(int op)
{
    switch (op)
    {
        case SQLITE_INDEX_CONSTRAINT_EQ:
            return true;
        case SQLITE_INDEX_CONSTRAINT_GT:
            return true;
        case SQLITE_INDEX_CONSTRAINT_LE:
            return true;
        case SQLITE_INDEX_CONSTRAINT_LT:
            return true;
        case SQLITE_INDEX_CONSTRAINT_GE:
            return true;
        case SQLITE_INDEX_CONSTRAINT_MATCH:
            return false;  // unhandled
#ifdef SQLITE_INDEX_CONSTRAINT_LIKE
        /* SQLite >= 3.10 */
        case SQLITE_INDEX_CONSTRAINT_LIKE:
            return true;
        case SQLITE_INDEX_CONSTRAINT_GLOB:
            return false;  // unhandled
        case SQLITE_INDEX_CONSTRAINT_REGEXP:
            return false;  // unhandled
#endif
#ifdef SQLITE_INDEX_CONSTRAINT_NE
            /* SQLite >= 3.21 */
        case SQLITE_INDEX_CONSTRAINT_NE:
            return true;
        case SQLITE_INDEX_CONSTRAINT_ISNOT:
            return false;  // OGR SQL only handles IS [NOT] NULL
        case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
            return true;
        case SQLITE_INDEX_CONSTRAINT_ISNULL:
            return true;
            ;
        case SQLITE_INDEX_CONSTRAINT_IS:
            return false;  // OGR SQL only handles IS [NOT] NULL
#endif
        default:
            break;
    }
    return false;
}

/************************************************************************/
/*                        OGR2SQLITE_BestIndex()                        */
/************************************************************************/

static int OGR2SQLITE_BestIndex(sqlite3_vtab *pVTab, sqlite3_index_info *pIndex)
{
    OGR2SQLITE_vtab *pMyVTab = reinterpret_cast<OGR2SQLITE_vtab *>(pVTab);
    OGRFeatureDefn *poFDefn = pMyVTab->poLayer->GetLayerDefn();

#ifdef DEBUG_OGR2SQLITE
    CPLString osQueryPatternUsable, osQueryPatternNotUsable;
    for (int i = 0; i < pIndex->nConstraint; i++)
    {
        int iCol = pIndex->aConstraint[i].iColumn;
        const char *pszFieldName = NULL;

        if (pMyVTab->bHasFIDColumn && iCol >= 0)
            --iCol;

        if (iCol == -1)
            pszFieldName = "FID";
        else if (iCol >= 0 && iCol < poFDefn->GetFieldCount())
            pszFieldName = poFDefn->GetFieldDefn(iCol)->GetNameRef();
        else
            pszFieldName = "unknown_field";

        const char *pszOp = NULL;
        switch (pIndex->aConstraint[i].op)
        {
            case SQLITE_INDEX_CONSTRAINT_EQ:
                pszOp = " = ";
                break;
            case SQLITE_INDEX_CONSTRAINT_GT:
                pszOp = " > ";
                break;
            case SQLITE_INDEX_CONSTRAINT_LE:
                pszOp = " <= ";
                break;
            case SQLITE_INDEX_CONSTRAINT_LT:
                pszOp = " < ";
                break;
            case SQLITE_INDEX_CONSTRAINT_GE:
                pszOp = " >= ";
                break;
            case SQLITE_INDEX_CONSTRAINT_MATCH:
                pszOp = " MATCH ";
                break;
#ifdef SQLITE_INDEX_CONSTRAINT_LIKE
            /* SQLite >= 3.10 */
            case SQLITE_INDEX_CONSTRAINT_LIKE:
                pszOp = " LIKE ";
                break;
            case SQLITE_INDEX_CONSTRAINT_GLOB:
                pszOp = " GLOB ";
                break;
            case SQLITE_INDEX_CONSTRAINT_REGEXP:
                pszOp = " REGEXP ";
                break;
#endif
#ifdef SQLITE_INDEX_CONSTRAINT_NE
            /* SQLite >= 3.21 */
            case SQLITE_INDEX_CONSTRAINT_NE:
                pszOp = " <> ";
                break;
            case SQLITE_INDEX_CONSTRAINT_ISNOT:
                pszOp = " IS NOT ";
                break;
            case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
                pszOp = " IS NOT NULL";
                break;
            case SQLITE_INDEX_CONSTRAINT_ISNULL:
                pszOp = " IS NULL";
                break;
            case SQLITE_INDEX_CONSTRAINT_IS:
                pszOp = " IS ";
                break;
#endif
            default:
                pszOp = " (unknown op) ";
                break;
        }

        if (pIndex->aConstraint[i].usable)
        {
            if (!osQueryPatternUsable.empty())
                osQueryPatternUsable += " AND ";
            osQueryPatternUsable += pszFieldName;
            osQueryPatternUsable += pszOp;
            osQueryPatternUsable += "?";
        }
        else
        {
            if (!osQueryPatternNotUsable.empty())
                osQueryPatternNotUsable += " AND ";
            osQueryPatternNotUsable += pszFieldName;
            osQueryPatternNotUsable += pszOp;
            osQueryPatternNotUsable += "?";
        }
    }
    CPLDebug("OGR2SQLITE", "BestIndex, usable ( %s ), not usable ( %s )",
             osQueryPatternUsable.c_str(), osQueryPatternNotUsable.c_str());
#endif

    int nConstraints = 0;
    for (int i = 0; i < pIndex->nConstraint; i++)
    {
        int iCol = pIndex->aConstraint[i].iColumn;

        if (pMyVTab->bHasFIDColumn && iCol >= 0)
            --iCol;

        if (pIndex->aConstraint[i].usable &&
            OGR2SQLITE_IsHandledOp(pIndex->aConstraint[i].op) &&
            iCol < poFDefn->GetFieldCount() &&
            (iCol < 0 || poFDefn->GetFieldDefn(iCol)->GetType() != OFTBinary))
        {
            pIndex->aConstraintUsage[i].argvIndex = nConstraints + 1;
            pIndex->aConstraintUsage[i].omit = true;

            nConstraints++;
        }
        else
        {
            pIndex->aConstraintUsage[i].argvIndex = 0;
            pIndex->aConstraintUsage[i].omit = false;
        }
    }

    int *panConstraints = nullptr;

    if (nConstraints)
    {
        panConstraints = static_cast<int *>(sqlite3_malloc(
            static_cast<int>(sizeof(int)) * (1 + 2 * nConstraints)));
        panConstraints[0] = nConstraints;

        nConstraints = 0;

        for (int i = 0; i < pIndex->nConstraint; i++)
        {
            if (pIndex->aConstraintUsage[i].omit)
            {
                panConstraints[2 * nConstraints + 1] =
                    pIndex->aConstraint[i].iColumn;
                panConstraints[2 * nConstraints + 2] =
                    pIndex->aConstraint[i].op;

                nConstraints++;
            }
        }
    }

    pIndex->orderByConsumed = false;
    pIndex->idxNum = 0;

    if (nConstraints != 0)
    {
        pIndex->idxStr = reinterpret_cast<char *>(panConstraints);
        pIndex->needToFreeIdxStr = true;
    }
    else
    {
        pIndex->idxStr = nullptr;
        pIndex->needToFreeIdxStr = false;
    }

    return SQLITE_OK;
}

/************************************************************************/
/*                      OGR2SQLITE_DisconnectDestroy()                  */
/************************************************************************/

static int OGR2SQLITE_DisconnectDestroy(sqlite3_vtab *pVTab)
{
    OGR2SQLITE_vtab *pMyVTab = reinterpret_cast<OGR2SQLITE_vtab *>(pVTab);

#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "DisconnectDestroy(%s)", pMyVTab->pszVTableName);
#endif

    sqlite3_free(pMyVTab->zErrMsg);
    if (pMyVTab->bCloseDS)
        pMyVTab->poDS->Release();
    pMyVTab->poModule->UnregisterVTable(pMyVTab->pszVTableName);
    CPLFree(pMyVTab->pszVTableName);
    CPLFree(pMyVTab);

    return SQLITE_OK;
}

/************************************************************************/
/*                           OGR2SQLITE_Open()                          */
/************************************************************************/

static int OGR2SQLITE_Open(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCursor)
{
    OGR2SQLITE_vtab *pMyVTab = reinterpret_cast<OGR2SQLITE_vtab *>(pVTab);
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Open(%s, %s)", pMyVTab->poDS->GetDescription(),
             pMyVTab->poLayer->GetDescription());
#endif

    GDALDataset *poDupDataSource = nullptr;
    OGRLayer *poLayer = nullptr;

    if (pMyVTab->nMyRef == 0)
    {
        poLayer = pMyVTab->poLayer;
    }
    else
    {
        poDupDataSource = GDALDataset::FromHandle(
            OGROpen(pMyVTab->poDS->GetDescription(), FALSE, nullptr));
        if (poDupDataSource == nullptr)
            return SQLITE_ERROR;
        poLayer = poDupDataSource->GetLayerByName(pMyVTab->poLayer->GetName());
        if (poLayer == nullptr)
        {
            delete poDupDataSource;
            return SQLITE_ERROR;
        }
        if (!poLayer->GetLayerDefn()->IsSame(pMyVTab->poLayer->GetLayerDefn()))
        {
            delete poDupDataSource;
            return SQLITE_ERROR;
        }
    }
    pMyVTab->nMyRef++;

    OGR2SQLITE_vtab_cursor *pCursor = static_cast<OGR2SQLITE_vtab_cursor *>(
        CPLCalloc(1, sizeof(OGR2SQLITE_vtab_cursor)));
    // We do not need to fill the non-extended fields.
    *ppCursor = reinterpret_cast<sqlite3_vtab_cursor *>(pCursor);

    pCursor->poDupDataSource = poDupDataSource;
    pCursor->poLayer = poLayer;
    pCursor->poLayer->ResetReading();
    pCursor->poFeature = nullptr;
    pCursor->nNextWishedIndex = 0;
    pCursor->nCurFeatureIndex = -1;
    pCursor->nFeatureCount = -1;

    pCursor->pabyGeomBLOB = nullptr;
    pCursor->nGeomBLOBLen = -1;

    return SQLITE_OK;
}

/************************************************************************/
/*                           OGR2SQLITE_Close()                         */
/************************************************************************/

static int OGR2SQLITE_Close(sqlite3_vtab_cursor *pCursor)
{
    OGR2SQLITE_vtab_cursor *pMyCursor =
        reinterpret_cast<OGR2SQLITE_vtab_cursor *>(pCursor);
    OGR2SQLITE_vtab *pMyVTab = pMyCursor->pVTab;
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Close(%s, %s)", pMyVTab->poDS->GetDescription(),
             pMyVTab->poLayer->GetDescription());
#endif
    pMyVTab->nMyRef--;

    delete pMyCursor->poFeature;
    delete pMyCursor->poDupDataSource;

    CPLFree(pMyCursor->pabyGeomBLOB);

    CPLFree(pCursor);

    return SQLITE_OK;
}

/************************************************************************/
/*                          OGR2SQLITE_Filter()                         */
/************************************************************************/

static int OGR2SQLITE_Filter(sqlite3_vtab_cursor *pCursor,
                             CPL_UNUSED int idxNum, const char *idxStr,
                             int argc, sqlite3_value **argv)
{
    OGR2SQLITE_vtab_cursor *pMyCursor =
        reinterpret_cast<OGR2SQLITE_vtab_cursor *>(pCursor);
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Filter");
#endif

    const int *panConstraints = reinterpret_cast<const int *>(idxStr);
    int nConstraints = panConstraints ? panConstraints[0] : 0;

    if (nConstraints != argc)
        return SQLITE_ERROR;

    CPLString osAttributeFilter;

    OGRFeatureDefn *poFDefn = pMyCursor->poLayer->GetLayerDefn();

    for (int i = 0; i < argc; i++)
    {
        int nCol = panConstraints[2 * i + 1];
        OGRFieldDefn *poFieldDefn = nullptr;

        if (pMyCursor->pVTab->bHasFIDColumn && nCol >= 0)
        {
            --nCol;
        }

        if (nCol >= 0)
        {
            poFieldDefn = poFDefn->GetFieldDefn(nCol);
            if (poFieldDefn == nullptr)
                return SQLITE_ERROR;
        }

        if (i != 0)
            osAttributeFilter += " AND ";

        if (poFieldDefn != nullptr)
        {
            const char *pszFieldName = poFieldDefn->GetNameRef();
            char ch = '\0';
            bool bNeedsQuoting = swq_is_reserved_keyword(pszFieldName) != 0;
            for (int j = 0; !bNeedsQuoting && (ch = pszFieldName[j]) != '\0';
                 j++)
            {
                if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
                    bNeedsQuoting = true;
            }

            if (bNeedsQuoting)
            {
                osAttributeFilter += '"';
                osAttributeFilter += SQLEscapeName(pszFieldName);
                osAttributeFilter += '"';
            }
            else
            {
                osAttributeFilter += pszFieldName;
            }
        }
        else
        {
            const char *pszSrcFIDColumn = pMyCursor->poLayer->GetFIDColumn();
            if (pszSrcFIDColumn && *pszSrcFIDColumn != '\0')
            {
                osAttributeFilter += '"';
                osAttributeFilter += SQLEscapeName(pszSrcFIDColumn);
                osAttributeFilter += '"';
            }
            else
            {
                osAttributeFilter += "FID";
            }
        }

        bool bExpectRightOperator = true;
        std::string osOp;
        const auto eSQLiteConstraintOp = panConstraints[2 * i + 2];
        switch (eSQLiteConstraintOp)
        {
            case SQLITE_INDEX_CONSTRAINT_EQ:
                osOp = " = ";
                break;
            case SQLITE_INDEX_CONSTRAINT_GT:
                osAttributeFilter += " > ";
                break;
            case SQLITE_INDEX_CONSTRAINT_LE:
                osAttributeFilter += " <= ";
                break;
            case SQLITE_INDEX_CONSTRAINT_LT:
                osAttributeFilter += " < ";
                break;
            case SQLITE_INDEX_CONSTRAINT_GE:
                osAttributeFilter += " >= ";
                break;
                // unhandled: SQLITE_INDEX_CONSTRAINT_MATCH
#ifdef SQLITE_INDEX_CONSTRAINT_LIKE
            /* SQLite >= 3.10 */
            case SQLITE_INDEX_CONSTRAINT_LIKE:
                osAttributeFilter += " LIKE ";
                break;
                // unhandled: SQLITE_INDEX_CONSTRAINT_GLOB
                // unhandled: SQLITE_INDEX_CONSTRAINT_REGEXP
#endif
#ifdef SQLITE_INDEX_CONSTRAINT_NE
            /* SQLite >= 3.21 */
            case SQLITE_INDEX_CONSTRAINT_NE:
                osAttributeFilter += " <> ";
                break;
            // case SQLITE_INDEX_CONSTRAINT_ISNOT: osAttributeFilter += " IS NOT
            // "; break;
            case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
                osAttributeFilter += " IS NOT NULL";
                bExpectRightOperator = false;
                break;
            case SQLITE_INDEX_CONSTRAINT_ISNULL:
                osAttributeFilter += " IS NULL";
                bExpectRightOperator = false;
                break;
                // case SQLITE_INDEX_CONSTRAINT_IS: osAttributeFilter += " IS ";
                // break;
#endif
            default:
            {
                sqlite3_free(pMyCursor->pVTab->zErrMsg);
                pMyCursor->pVTab->zErrMsg =
                    sqlite3_mprintf("Unhandled constraint operator : %d",
                                    panConstraints[2 * i + 2]);
                return SQLITE_ERROR;
            }
        }

        if (bExpectRightOperator)
        {
            const auto eSQLiteType = sqlite3_value_type(argv[i]);
            if (eSQLiteType == SQLITE_INTEGER)
            {
                osAttributeFilter += osOp;
                osAttributeFilter +=
                    CPLSPrintf(CPL_FRMT_GIB, sqlite3_value_int64(argv[i]));
            }
            else if (eSQLiteType == SQLITE_FLOAT)
            {  // Insure that only Decimal.Points are used, never local settings
                // such as Decimal.Comma.
                osAttributeFilter += osOp;
                osAttributeFilter +=
                    CPLSPrintf("%.17g", sqlite3_value_double(argv[i]));
            }
            else if (eSQLiteType == SQLITE_TEXT)
            {
                osAttributeFilter += osOp;
                osAttributeFilter += "'";
                osAttributeFilter +=
                    SQLEscapeLiteral(reinterpret_cast<const char *>(
                        sqlite3_value_text(argv[i])));
                osAttributeFilter += "'";
            }
            else if (eSQLiteConstraintOp == SQLITE_INDEX_CONSTRAINT_EQ &&
                     eSQLiteType == SQLITE_NULL)
            {
                osAttributeFilter += " IN (NULL)";
            }
            else
            {
                sqlite3_free(pMyCursor->pVTab->zErrMsg);
                pMyCursor->pVTab->zErrMsg = sqlite3_mprintf(
                    "Unhandled constraint data type : %d", eSQLiteType);
                return SQLITE_ERROR;
            }
        }
    }

#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Attribute filter : %s", osAttributeFilter.c_str());
#endif

    if (pMyCursor->poLayer->SetAttributeFilter(!osAttributeFilter.empty()
                                                   ? osAttributeFilter.c_str()
                                                   : nullptr) != OGRERR_NONE)
    {
        sqlite3_free(pMyCursor->pVTab->zErrMsg);
        pMyCursor->pVTab->zErrMsg = sqlite3_mprintf(
            "Cannot apply attribute filter : %s", osAttributeFilter.c_str());
        return SQLITE_ERROR;
    }

    if (pMyCursor->poLayer->TestCapability(OLCFastFeatureCount))
        pMyCursor->nFeatureCount = pMyCursor->poLayer->GetFeatureCount();
    else
        pMyCursor->nFeatureCount = -1;
    pMyCursor->poLayer->ResetReading();

    if (pMyCursor->nFeatureCount < 0)
    {
        pMyCursor->poFeature = pMyCursor->poLayer->GetNextFeature();
#ifdef DEBUG_OGR2SQLITE
        CPLDebug("OGR2SQLITE", "GetNextFeature() --> " CPL_FRMT_GIB,
                 pMyCursor->poFeature ? pMyCursor->poFeature->GetFID() : -1);
#endif
    }

    pMyCursor->nNextWishedIndex = 0;
    pMyCursor->nCurFeatureIndex = -1;

    return SQLITE_OK;
}

/************************************************************************/
/*                          OGR2SQLITE_Next()                           */
/************************************************************************/

static int OGR2SQLITE_Next(sqlite3_vtab_cursor *pCursor)
{
    OGR2SQLITE_vtab_cursor *pMyCursor =
        reinterpret_cast<OGR2SQLITE_vtab_cursor *>(pCursor);
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Next");
#endif

    pMyCursor->nNextWishedIndex++;
    if (pMyCursor->nFeatureCount < 0)
    {
        delete pMyCursor->poFeature;
        pMyCursor->poFeature = pMyCursor->poLayer->GetNextFeature();

        CPLFree(pMyCursor->pabyGeomBLOB);
        pMyCursor->pabyGeomBLOB = nullptr;
        pMyCursor->nGeomBLOBLen = -1;

#ifdef DEBUG_OGR2SQLITE
        CPLDebug("OGR2SQLITE", "GetNextFeature() --> " CPL_FRMT_GIB,
                 pMyCursor->poFeature ? pMyCursor->poFeature->GetFID() : -1);
#endif
    }
    return SQLITE_OK;
}

/************************************************************************/
/*                          OGR2SQLITE_Eof()                            */
/************************************************************************/

static int OGR2SQLITE_Eof(sqlite3_vtab_cursor *pCursor)
{
    OGR2SQLITE_vtab_cursor *pMyCursor =
        reinterpret_cast<OGR2SQLITE_vtab_cursor *>(pCursor);
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Eof");
#endif

    if (pMyCursor->nFeatureCount < 0)
    {
        return pMyCursor->poFeature == nullptr;
    }
    else
    {
        return pMyCursor->nNextWishedIndex >= pMyCursor->nFeatureCount;
    }
}

/************************************************************************/
/*                      OGR2SQLITE_GoToWishedIndex()                    */
/************************************************************************/

static void OGR2SQLITE_GoToWishedIndex(OGR2SQLITE_vtab_cursor *pMyCursor)
{
    if (pMyCursor->nFeatureCount >= 0)
    {
        if (pMyCursor->nCurFeatureIndex < pMyCursor->nNextWishedIndex)
        {
            do
            {
                pMyCursor->nCurFeatureIndex++;

                delete pMyCursor->poFeature;
                pMyCursor->poFeature = pMyCursor->poLayer->GetNextFeature();
#ifdef DEBUG_OGR2SQLITE
                CPLDebug("OGR2SQLITE", "GetNextFeature() --> " CPL_FRMT_GIB,
                         pMyCursor->poFeature ? pMyCursor->poFeature->GetFID()
                                              : -1);
#endif
            } while (pMyCursor->nCurFeatureIndex < pMyCursor->nNextWishedIndex);

            CPLFree(pMyCursor->pabyGeomBLOB);
            pMyCursor->pabyGeomBLOB = nullptr;
            pMyCursor->nGeomBLOBLen = -1;
        }
    }
}

/************************************************************************/
/*                    OGR2SQLITE_ExportGeometry()                       */
/************************************************************************/

static void OGR2SQLITE_ExportGeometry(OGRGeometry *poGeom, int nSRSId,
                                      GByte *&pabyGeomBLOB, int &nGeomBLOBLen)
{
    if (OGRSQLiteLayer::ExportSpatiaLiteGeometry(poGeom, nSRSId, wkbNDR, FALSE,
                                                 FALSE, &pabyGeomBLOB,
                                                 &nGeomBLOBLen) != OGRERR_NONE)
    {
        nGeomBLOBLen = 0;
    }
    /* This is a hack: we add the original curve geometry after */
    /* the spatialite blob */
    else if (poGeom->hasCurveGeometry())
    {
        const size_t nWkbSize = poGeom->WkbSize();
        if (nWkbSize + 1 >
            static_cast<size_t>(std::numeric_limits<int>::max()) - nGeomBLOBLen)
        {
            CPLError(CE_Failure, CPLE_NotSupported, "Too large geometry");
            nGeomBLOBLen = 0;
            return;
        }

        pabyGeomBLOB = static_cast<GByte *>(
            CPLRealloc(pabyGeomBLOB, nGeomBLOBLen + nWkbSize + 1));
        poGeom->exportToWkb(wkbNDR, pabyGeomBLOB + nGeomBLOBLen, wkbVariantIso);
        /* Cheat a bit and add a end-of-blob spatialite marker */
        pabyGeomBLOB[nGeomBLOBLen + nWkbSize] = 0xFE;
        nGeomBLOBLen += static_cast<int>(nWkbSize) + 1;
    }
}

/************************************************************************/
/*                         OGR2SQLITE_Column()                          */
/************************************************************************/

static int OGR2SQLITE_Column(sqlite3_vtab_cursor *pCursor,
                             sqlite3_context *pContext, int nCol)
{
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Column %d", nCol);
#endif

    OGR2SQLITE_vtab_cursor *pMyCursor =
        reinterpret_cast<OGR2SQLITE_vtab_cursor *>(pCursor);

    OGR2SQLITE_GoToWishedIndex(pMyCursor);

    OGRFeature *poFeature = pMyCursor->poFeature;
    if (poFeature == nullptr)
        return SQLITE_ERROR;

    if (pMyCursor->pVTab->bHasFIDColumn)
    {
        if (nCol == 0)
        {
            sqlite3_result_int64(pContext, poFeature->GetFID());
            return SQLITE_OK;
        }
        --nCol;
    }

    OGRFeatureDefn *poFDefn = pMyCursor->poLayer->GetLayerDefn();
    int nFieldCount = poFDefn->GetFieldCount();

    if (nCol == nFieldCount)
    {
        sqlite3_result_text(pContext, poFeature->GetStyleString(), -1,
                            SQLITE_TRANSIENT);
        return SQLITE_OK;
    }
    else if (nCol == (nFieldCount + 1) && poFDefn->GetGeomType() != wkbNone)
    {
        if (pMyCursor->nGeomBLOBLen < 0)
        {
            OGRGeometry *poGeom = poFeature->GetGeometryRef();
            if (poGeom == nullptr)
            {
                pMyCursor->nGeomBLOBLen = 0;
            }
            else
            {
                CPLAssert(pMyCursor->pabyGeomBLOB == nullptr);

                const OGRSpatialReference *poSRS =
                    poGeom->getSpatialReference();
                int nSRSId = pMyCursor->pVTab->poModule->FetchSRSId(poSRS);

                OGR2SQLITE_ExportGeometry(poGeom, nSRSId,
                                          pMyCursor->pabyGeomBLOB,
                                          pMyCursor->nGeomBLOBLen);
            }
        }

        if (pMyCursor->nGeomBLOBLen == 0)
        {
            sqlite3_result_null(pContext);
        }
        else
        {
            GByte *pabyGeomBLOBDup =
                static_cast<GByte *>(CPLMalloc(pMyCursor->nGeomBLOBLen));
            memcpy(pabyGeomBLOBDup, pMyCursor->pabyGeomBLOB,
                   pMyCursor->nGeomBLOBLen);
            sqlite3_result_blob(pContext, pabyGeomBLOBDup,
                                pMyCursor->nGeomBLOBLen, CPLFree);
        }

        return SQLITE_OK;
    }
    else if (nCol > (nFieldCount + 1) &&
             nCol - (nFieldCount + 1) < poFDefn->GetGeomFieldCount())
    {
        OGRGeometry *poGeom =
            poFeature->GetGeomFieldRef(nCol - (nFieldCount + 1));
        if (poGeom == nullptr)
        {
            sqlite3_result_null(pContext);
        }
        else
        {
            const OGRSpatialReference *poSRS = poGeom->getSpatialReference();
            int nSRSId = pMyCursor->pVTab->poModule->FetchSRSId(poSRS);

            GByte *pabyGeomBLOB = nullptr;
            int nGeomBLOBLen = 0;
            OGR2SQLITE_ExportGeometry(poGeom, nSRSId, pabyGeomBLOB,
                                      nGeomBLOBLen);

            if (nGeomBLOBLen == 0)
            {
                sqlite3_result_null(pContext);
            }
            else
            {
                sqlite3_result_blob(pContext, pabyGeomBLOB, nGeomBLOBLen,
                                    CPLFree);
            }
        }
        return SQLITE_OK;
    }
    else if (nCol == nFieldCount + 1 + poFDefn->GetGeomFieldCount())
    {
        sqlite3_result_text(pContext, poFeature->GetNativeData(), -1,
                            SQLITE_TRANSIENT);
        return SQLITE_OK;
    }
    else if (nCol == nFieldCount + 1 + poFDefn->GetGeomFieldCount() + 1)
    {
        sqlite3_result_text(pContext, poFeature->GetNativeMediaType(), -1,
                            SQLITE_TRANSIENT);
        return SQLITE_OK;
    }
    else if (nCol < 0 ||
             nCol >= nFieldCount + 1 + poFDefn->GetGeomFieldCount() + 2)
    {
        return SQLITE_ERROR;
    }
    else if (!poFeature->IsFieldSetAndNotNull(nCol))
    {
        sqlite3_result_null(pContext);
        return SQLITE_OK;
    }

    switch (poFDefn->GetFieldDefn(nCol)->GetType())
    {
        case OFTInteger:
            sqlite3_result_int(pContext, poFeature->GetFieldAsInteger(nCol));
            break;

        case OFTInteger64:
            sqlite3_result_int64(pContext,
                                 poFeature->GetFieldAsInteger64(nCol));
            break;

        case OFTReal:
            sqlite3_result_double(pContext, poFeature->GetFieldAsDouble(nCol));
            break;

        case OFTBinary:
        {
            int nSize = 0;
            GByte *pBlob = poFeature->GetFieldAsBinary(nCol, &nSize);
            sqlite3_result_blob(pContext, pBlob, nSize, SQLITE_TRANSIENT);
            break;
        }

        case OFTDateTime:
        {
            char *pszStr = OGRGetXMLDateTime(poFeature->GetRawFieldRef(nCol));
            sqlite3_result_text(pContext, pszStr, -1, SQLITE_TRANSIENT);
            CPLFree(pszStr);
            break;
        }

        case OFTDate:
        {
            int nYear, nMonth, nDay, nHour, nMinute, nSecond, nTZ;
            poFeature->GetFieldAsDateTime(nCol, &nYear, &nMonth, &nDay, &nHour,
                                          &nMinute, &nSecond, &nTZ);
            char szBuffer[64];
            snprintf(szBuffer, sizeof(szBuffer), "%04d-%02d-%02d", nYear,
                     nMonth, nDay);
            sqlite3_result_text(pContext, szBuffer, -1, SQLITE_TRANSIENT);
            break;
        }

        case OFTTime:
        {
            int nYear = 0;
            int nMonth = 0;
            int nDay = 0;
            int nHour = 0;
            int nMinute = 0;
            int nTZ = 0;
            float fSecond = 0.0f;
            poFeature->GetFieldAsDateTime(nCol, &nYear, &nMonth, &nDay, &nHour,
                                          &nMinute, &fSecond, &nTZ);
            char szBuffer[64];
            if (OGR_GET_MS(fSecond) != 0)
                snprintf(szBuffer, sizeof(szBuffer), "%02d:%02d:%06.3f", nHour,
                         nMinute, fSecond);
            else
                snprintf(szBuffer, sizeof(szBuffer), "%02d:%02d:%02d", nHour,
                         nMinute, static_cast<int>(fSecond));
            sqlite3_result_text(pContext, szBuffer, -1, SQLITE_TRANSIENT);
            break;
        }

        default:
            sqlite3_result_text(pContext, poFeature->GetFieldAsString(nCol), -1,
                                SQLITE_TRANSIENT);
            break;
    }

    return SQLITE_OK;
}

/************************************************************************/
/*                         OGR2SQLITE_Rowid()                           */
/************************************************************************/

static int OGR2SQLITE_Rowid(sqlite3_vtab_cursor *pCursor, sqlite3_int64 *pRowid)
{
    OGR2SQLITE_vtab_cursor *pMyCursor =
        reinterpret_cast<OGR2SQLITE_vtab_cursor *>(pCursor);
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Rowid");
#endif

    OGR2SQLITE_GoToWishedIndex(pMyCursor);

    if (pMyCursor->poFeature == nullptr)
        return SQLITE_ERROR;

    *pRowid = pMyCursor->poFeature->GetFID();

    return SQLITE_OK;
}

/************************************************************************/
/*                         OGR2SQLITE_Rename()                          */
/************************************************************************/

static int OGR2SQLITE_Rename(CPL_UNUSED sqlite3_vtab *pVtab,
                             CPL_UNUSED const char *zNew)
{
    // CPLDebug("OGR2SQLITE", "Rename");
    return SQLITE_ERROR;
}

#if 0
/************************************************************************/
/*                        OGR2SQLITE_FindFunction()                     */
/************************************************************************/

static
int OGR2SQLITE_FindFunction(sqlite3_vtab *pVtab,
                            int nArg,
                            const char *zName,
                            void (**pxFunc)(sqlite3_context*,int,sqlite3_value**),
                            void **ppArg)
{
    CPLDebug("OGR2SQLITE", "FindFunction %s", zName);

    return 0;
}
#endif

/************************************************************************/
/*                     OGR2SQLITE_FeatureFromArgs()                     */
/************************************************************************/

static OGRFeature *OGR2SQLITE_FeatureFromArgs(OGR2SQLITE_vtab *pMyVTab,
                                              int argc, sqlite3_value **argv)
{
    OGRLayer *poLayer = pMyVTab->poLayer;
    OGRFeatureDefn *poLayerDefn = poLayer->GetLayerDefn();
    const int nFieldCount = poLayerDefn->GetFieldCount();
    const int nGeomFieldCount = poLayerDefn->GetGeomFieldCount();
    // The argv[0] parameter is the rowid of a row in the virtual table to be deleted.
    // The argv[1] parameter is the rowid of a new row to be inserted into the virtual table
    // If bHasFIDColumn, we have an extra column, before the attributes
    const int nLeadingColumns = pMyVTab->bHasFIDColumn ? 3 : 2;
    if (argc != nLeadingColumns + nFieldCount + 1 + /* OGR_STYLE */
                    nGeomFieldCount + 2 /* NativeData and NativeMediaType */)
    {
        CPLDebug("OGR2SQLITE", "Did not get expect argument count : %d, %d",
                 argc, nLeadingColumns + nFieldCount + 1 + nGeomFieldCount + 2);
        return nullptr;
    }

    auto poFeature = std::make_unique<OGRFeature>(poLayerDefn);

    if (pMyVTab->bHasFIDColumn)
    {
        if (sqlite3_value_type(argv[2]) == SQLITE_INTEGER)
        {
            if (sqlite3_value_type(argv[1]) == SQLITE_INTEGER &&
                sqlite3_value_int64(argv[1]) != sqlite3_value_int64(argv[2]))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Value provided through ROWID and %s are different",
                         poLayer->GetFIDColumn());
                return nullptr;
            }
            poFeature->SetFID(sqlite3_value_int64(argv[2]));
        }
    }
    else if (sqlite3_value_type(argv[1]) == SQLITE_INTEGER)
    {
        poFeature->SetFID(sqlite3_value_int64(argv[1]));
    }

    int iArgc = nLeadingColumns;
    for (int i = 0; i < nFieldCount; i++, ++iArgc)
    {
        switch (sqlite3_value_type(argv[iArgc]))
        {
            case SQLITE_NULL:
                poFeature->SetFieldNull(i);
                break;
            case SQLITE_INTEGER:
                poFeature->SetField(i, sqlite3_value_int64(argv[iArgc]));
                break;
            case SQLITE_FLOAT:
                poFeature->SetField(i, sqlite3_value_double(argv[iArgc]));
                break;
            case SQLITE_TEXT:
            {
                const char *pszValue = reinterpret_cast<const char *>(
                    sqlite3_value_text(argv[iArgc]));
                switch (poLayerDefn->GetFieldDefn(i)->GetType())
                {
                    case OFTDate:
                    case OFTTime:
                    case OFTDateTime:
                        if (!OGRParseDate(pszValue,
                                          poFeature->GetRawFieldRef(i), 0))
                            poFeature->SetField(i, pszValue);
                        break;

                    default:
                        poFeature->SetField(i, pszValue);
                        break;
                }
                break;
            }
            case SQLITE_BLOB:
            {
                GByte *paby = reinterpret_cast<GByte *>(
                    const_cast<void *>(sqlite3_value_blob(argv[iArgc])));
                int nLen = sqlite3_value_bytes(argv[iArgc]);
                poFeature->SetField(i, nLen, paby);
                break;
            }
            default:
                break;
        }
    }

    if (sqlite3_value_type(argv[iArgc]) == SQLITE_TEXT)
    {
        poFeature->SetStyleString(
            reinterpret_cast<const char *>(sqlite3_value_text(argv[iArgc])));
    }
    ++iArgc;

    for (int i = 0; i < nGeomFieldCount; i++, ++iArgc)
    {
        if (sqlite3_value_type(argv[iArgc]) == SQLITE_BLOB)
        {
            const GByte *pabyBlob = reinterpret_cast<const GByte *>(
                sqlite3_value_blob(argv[iArgc]));
            int nLen = sqlite3_value_bytes(argv[iArgc]);
            OGRGeometry *poGeom = nullptr;
            if (OGRSQLiteLayer::ImportSpatiaLiteGeometry(
                    pabyBlob, nLen, &poGeom) == OGRERR_NONE)
            {
                /*                OGRwkbGeometryType eGeomFieldType =
                                    poFeature->GetDefnRef()->GetGeomFieldDefn(i)->GetType();
                                if( OGR_GT_IsCurve(eGeomFieldType) &&
                   !OGR_GT_IsCurve(poGeom->getGeometryType()) )
                                {
                                    OGRGeometry* poCurveGeom =
                   poGeom->getCurveGeometry();
                                    poFeature->SetGeomFieldDirectly(i,
                   poCurveGeom); delete poCurveGeom;
                                }
                                else*/
                poFeature->SetGeomFieldDirectly(i, poGeom);
            }
        }
    }

    if (sqlite3_value_type(argv[iArgc]) == SQLITE_TEXT)
    {
        poFeature->SetNativeData(
            reinterpret_cast<const char *>(sqlite3_value_text(argv[iArgc])));
    }
    ++iArgc;

    if (sqlite3_value_type(argv[iArgc]) == SQLITE_TEXT)
    {
        poFeature->SetNativeMediaType(
            reinterpret_cast<const char *>(sqlite3_value_text(argv[iArgc])));
    }

    return poFeature.release();
}

/************************************************************************/
/*                            OGR2SQLITE_Update()                       */
/************************************************************************/

static int OGR2SQLITE_Update(sqlite3_vtab *pVTab, int argc,
                             sqlite3_value **argv, sqlite_int64 *pRowid)
{
    CPLDebug("OGR2SQLITE", "OGR2SQLITE_Update");

    OGR2SQLITE_vtab *pMyVTab = reinterpret_cast<OGR2SQLITE_vtab *>(pVTab);
    OGRLayer *poLayer = pMyVTab->poLayer;

    if (argc == 1)
    {
        /* DELETE */

        OGRErr eErr = poLayer->DeleteFeature(sqlite3_value_int64(argv[0]));

        return (eErr == OGRERR_NONE) ? SQLITE_OK : SQLITE_ERROR;
    }
    else if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL)
    {
        /* INSERT */

        OGRFeature *poFeature = OGR2SQLITE_FeatureFromArgs(pMyVTab, argc, argv);
        if (poFeature == nullptr)
            return SQLITE_ERROR;

        OGRErr eErr = poLayer->CreateFeature(poFeature);
        if (eErr == OGRERR_NONE)
            *pRowid = poFeature->GetFID();

        delete poFeature;

        return (eErr == OGRERR_NONE) ? SQLITE_OK : SQLITE_ERROR;
    }
    else if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_INTEGER &&
             sqlite3_value_type(argv[1]) == SQLITE_INTEGER &&
             sqlite3_value_int64(argv[0]) == sqlite3_value_int64(argv[1]))
    {
        /* UPDATE */

        OGRFeature *poFeature = OGR2SQLITE_FeatureFromArgs(pMyVTab, argc, argv);
        if (poFeature == nullptr)
            return SQLITE_ERROR;

        OGRErr eErr = poLayer->SetFeature(poFeature);

        delete poFeature;

        return (eErr == OGRERR_NONE) ? SQLITE_OK : SQLITE_ERROR;
    }

    // UPDATE table SET rowid=rowid+1 WHERE ... unsupported

    return SQLITE_ERROR;
}

/************************************************************************/
/*                        sOGR2SQLITEModule                             */
/************************************************************************/

static const struct sqlite3_module sOGR2SQLITEModule = {
    1,                        /* iVersion */
    OGR2SQLITE_ConnectCreate, /* xCreate */
    OGR2SQLITE_ConnectCreate, /* xConnect */
    OGR2SQLITE_BestIndex,
    OGR2SQLITE_DisconnectDestroy, /* xDisconnect */
    OGR2SQLITE_DisconnectDestroy, /* xDestroy */
    OGR2SQLITE_Open,
    OGR2SQLITE_Close,
    OGR2SQLITE_Filter,
    OGR2SQLITE_Next,
    OGR2SQLITE_Eof,
    OGR2SQLITE_Column,
    OGR2SQLITE_Rowid,
    OGR2SQLITE_Update,
    nullptr, /* xBegin */
    nullptr, /* xSync */
    nullptr, /* xCommit */
    nullptr, /* xFindFunctionRollback */
    nullptr,
    /* xFindFunction */  // OGR2SQLITE_FindFunction;
    OGR2SQLITE_Rename,
    nullptr,  // xSavepoint
    nullptr,  // xRelease
    nullptr,  // xRollbackTo
    nullptr,  // xShadowName
#if SQLITE_VERSION_NUMBER >=                                                   \
    3044000L /* should be the first version with the below symbols */
    nullptr,  // xIntegrity
#endif
};

/************************************************************************/
/*                           OGR2SQLITE_GetLayer()                      */
/************************************************************************/

static OGRLayer *OGR2SQLITE_GetLayer(const char *pszFuncName,
                                     sqlite3_context *pContext, int argc,
                                     sqlite3_value **argv)
{
    if (argc != 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s: %s(): %s", "VirtualOGR",
                 pszFuncName, "Invalid number of arguments");
        sqlite3_result_null(pContext);
        return nullptr;
    }

    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s: %s(): %s", "VirtualOGR",
                 pszFuncName, "Invalid argument type");
        sqlite3_result_null(pContext);
        return nullptr;
    }

    const char *pszVTableName =
        reinterpret_cast<const char *>(sqlite3_value_text(argv[0]));

    OGR2SQLITEModule *poModule =
        static_cast<OGR2SQLITEModule *>(sqlite3_user_data(pContext));

    OGRLayer *poLayer = poModule->GetLayerForVTable(SQLUnescape(pszVTableName));
    if (poLayer == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s: %s(): %s", "VirtualOGR",
                 pszFuncName, "Unknown virtual table");
        sqlite3_result_null(pContext);
        return nullptr;
    }

    return poLayer;
}

/************************************************************************/
/*                       OGR2SQLITE_ogr_layer_Extent()                  */
/************************************************************************/

static void OGR2SQLITE_ogr_layer_Extent(sqlite3_context *pContext, int argc,
                                        sqlite3_value **argv)
{
    OGRLayer *poLayer =
        OGR2SQLITE_GetLayer("ogr_layer_Extent", pContext, argc, argv);
    if (poLayer == nullptr)
        return;

    OGR2SQLITEModule *poModule =
        static_cast<OGR2SQLITEModule *>(sqlite3_user_data(pContext));

    if (poLayer->GetGeomType() == wkbNone)
    {
        sqlite3_result_null(pContext);
        return;
    }

    OGREnvelope sExtent;
    if (poLayer->GetExtent(&sExtent) != OGRERR_NONE)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s: %s(): %s", "VirtualOGR",
                 "ogr_layer_Extent", "Cannot fetch layer extent");
        sqlite3_result_null(pContext);
        return;
    }

    OGRPolygon oPoly;
    OGRLinearRing *poRing = new OGRLinearRing();
    oPoly.addRingDirectly(poRing);
    poRing->addPoint(sExtent.MinX, sExtent.MinY);
    poRing->addPoint(sExtent.MaxX, sExtent.MinY);
    poRing->addPoint(sExtent.MaxX, sExtent.MaxY);
    poRing->addPoint(sExtent.MinX, sExtent.MaxY);
    poRing->addPoint(sExtent.MinX, sExtent.MinY);

    GByte *pabySLBLOB = nullptr;
    int nBLOBLen = 0;
    int nSRID = poModule->FetchSRSId(poLayer->GetSpatialRef());
    if (OGRSQLiteLayer::ExportSpatiaLiteGeometry(&oPoly, nSRID, wkbNDR, FALSE,
                                                 FALSE, &pabySLBLOB,
                                                 &nBLOBLen) == OGRERR_NONE)
    {
        sqlite3_result_blob(pContext, pabySLBLOB, nBLOBLen, CPLFree);
    }
    else
    {
        sqlite3_result_null(pContext);
    }
}

/************************************************************************/
/*                       OGR2SQLITE_ogr_layer_SRID()                    */
/************************************************************************/

static void OGR2SQLITE_ogr_layer_SRID(sqlite3_context *pContext, int argc,
                                      sqlite3_value **argv)
{
    OGRLayer *poLayer =
        OGR2SQLITE_GetLayer("OGR2SQLITE_ogr_layer_SRID", pContext, argc, argv);
    if (poLayer == nullptr)
        return;

    OGR2SQLITEModule *poModule =
        static_cast<OGR2SQLITEModule *>(sqlite3_user_data(pContext));

    if (poLayer->GetGeomType() == wkbNone)
    {
        sqlite3_result_null(pContext);
        return;
    }

    int nSRID = poModule->FetchSRSId(poLayer->GetSpatialRef());
    sqlite3_result_int(pContext, nSRID);
}

/************************************************************************/
/*                 OGR2SQLITE_ogr_layer_GeometryType()                  */
/************************************************************************/

static void OGR2SQLITE_ogr_layer_GeometryType(sqlite3_context *pContext,
                                              int argc, sqlite3_value **argv)
{
    OGRLayer *poLayer = OGR2SQLITE_GetLayer("OGR2SQLITE_ogr_layer_GeometryType",
                                            pContext, argc, argv);
    if (poLayer == nullptr)
        return;

    OGRwkbGeometryType eType = poLayer->GetGeomType();

    if (eType == wkbNone)
    {
        sqlite3_result_null(pContext);
        return;
    }

    const char *psz2DName = OGRToOGCGeomType(eType);
    if (wkbHasZ(eType))
        sqlite3_result_text(pContext, CPLSPrintf("%s Z", psz2DName), -1,
                            SQLITE_TRANSIENT);
    else
        sqlite3_result_text(pContext, psz2DName, -1, SQLITE_TRANSIENT);
}

/************************************************************************/
/*                OGR2SQLITE_ogr_layer_FeatureCount()                   */
/************************************************************************/

static void OGR2SQLITE_ogr_layer_FeatureCount(sqlite3_context *pContext,
                                              int argc, sqlite3_value **argv)
{
    OGRLayer *poLayer = OGR2SQLITE_GetLayer("OGR2SQLITE_ogr_layer_FeatureCount",
                                            pContext, argc, argv);
    if (poLayer == nullptr)
        return;

    sqlite3_result_int64(pContext, poLayer->GetFeatureCount());
}

/************************************************************************/
/*                      OGR2SQLITEDestroyModule()                       */
/************************************************************************/

static void OGR2SQLITEDestroyModule(void *pData)
{
    // Comment out this debug message, as the module can be registered in the
    // connection of proj.db that is since PROJ 8.1 a cache that is destroyed at
    // PROJ unloading, after GDAL itself has cleaned up itself. CPLDebug("OGR",
    // "Unloading VirtualOGR module");
    delete static_cast<OGR2SQLITEModule *>(pData);
}

/* ENABLE_VIRTUAL_OGR_SPATIAL_INDEX is not defined */
#ifdef ENABLE_VIRTUAL_OGR_SPATIAL_INDEX

/************************************************************************/
/*                    OGR2SQLITESpatialIndex_vtab                       */
/************************************************************************/

typedef struct
{
    /* Mandatory fields by sqlite3: don't change or reorder them ! */
    const sqlite3_module *pModule;
    int nRef;
    char *zErrMsg;

    /* Extension fields */
    char *pszVTableName;
    OGR2SQLITEModule *poModule;
    GDALDataset *poDS;
    int bCloseDS;
    OGRLayer *poLayer;
    int nMyRef;
} OGR2SQLITESpatialIndex_vtab;

/************************************************************************/
/*                  OGR2SQLITESpatialIndex_vtab_cursor                  */
/************************************************************************/

typedef struct
{
    /* Mandatory fields by sqlite3: don't change or reorder them ! */
    OGR2SQLITESpatialIndex_vtab *pVTab;

    /* Extension fields */
    GDALDataset *poDupDataSource;
    OGRLayer *poLayer;
    OGRFeature *poFeature;
    int bHasSetBounds;
    double dfMinX;
    double dfMinY;
    double dfMaxX;
    double dfMaxY;
} OGR2SQLITESpatialIndex_vtab_cursor;

/************************************************************************/
/*                   OGR2SQLITESpatialIndex_ConnectCreate()             */
/************************************************************************/

static int OGR2SQLITESpatialIndex_ConnectCreate(sqlite3 *hDB, void *pAux,
                                                int argc,
                                                const char *const *argv,
                                                sqlite3_vtab **ppVTab,
                                                char **pzErr)
{
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "ConnectCreate(%s)", argv[2]);
#endif

    OGR2SQLITEModule *poModule = (OGR2SQLITEModule *)pAux;

    /* -------------------------------------------------------------------- */
    /*      If called from ogrexecutesql.cpp                                */
    /* -------------------------------------------------------------------- */
    GDALDataset *poDS = poModule->GetDS();
    if (poDS == NULL)
        return SQLITE_ERROR;

    if (argc != 10)
    {
        *pzErr =
            sqlite3_mprintf("Expected syntax: CREATE VIRTUAL TABLE xxx USING "
                            "VirtualOGRSpatialIndex(ds_idx, layer_name, pkid, "
                            "xmin, xmax, ymin, ymax)");
        return SQLITE_ERROR;
    }

    int nDSIndex = atoi(argv[3]);
    if (nDSIndex >= 0)
    {
        poDS = poModule->GetExtraDS(nDSIndex);
        if (poDS == NULL)
        {
            *pzErr = sqlite3_mprintf("Invalid dataset index : %d", nDSIndex);
            return SQLITE_ERROR;
        }
    }

    poDS = (GDALDataset *)OGROpen(poDS->GetName(), FALSE, NULL);
    if (poDS == NULL)
    {
        return SQLITE_ERROR;
    }

    CPLString osLayerName(SQLUnescape(argv[4]));

    OGRLayer *poLayer = poDS->GetLayerByName(osLayerName);
    if (poLayer == NULL)
    {
        *pzErr = sqlite3_mprintf("Cannot find layer '%s' in '%s'",
                                 osLayerName.c_str(), poDS->GetName());
        return SQLITE_ERROR;
    }

    OGR2SQLITESpatialIndex_vtab *vtab =
        (OGR2SQLITESpatialIndex_vtab *)CPLCalloc(
            1, sizeof(OGR2SQLITESpatialIndex_vtab));
    // We do not need to fill the non-extended fields.
    vtab->pszVTableName = CPLStrdup(SQLEscapeName(argv[2]));
    vtab->poModule = poModule;
    vtab->poDS = poDS;
    vtab->bCloseDS = true;
    vtab->poLayer = poLayer;
    vtab->nMyRef = 0;

    *ppVTab = (sqlite3_vtab *)vtab;

    CPLString osSQL;
    osSQL = "CREATE TABLE ";
    osSQL += "\"";
    osSQL += SQLEscapeName(argv[2]);
    osSQL += "\"";
    osSQL += "(";

    bool bAddComma = false;

    for (i = 0; i < 5; i++)
    {
        if (bAddComma)
            osSQL += ",";
        bAddComma = true;

        osSQL += "\"";
        osSQL += SQLEscapeName(SQLUnescape(argv[5 + i]));
        osSQL += "\"";
        osSQL += " ";
        osSQL += (i == 0) ? "INTEGER" : "FLOAT";
    }

    osSQL += ")";

    CPLDebug("OGR2SQLITE", "sqlite3_declare_vtab(%s)", osSQL.c_str());
    if (sqlite3_declare_vtab(hDB, osSQL.c_str()) != SQLITE_OK)
    {
        *pzErr = sqlite3_mprintf("CREATE VIRTUAL: invalid SQL statement : %s",
                                 osSQL.c_str());
        return SQLITE_ERROR;
    }

    return SQLITE_OK;
}

/************************************************************************/
/*                      OGR2SQLITESpatialIndex_BestIndex()              */
/************************************************************************/

static int OGR2SQLITESpatialIndex_BestIndex(sqlite3_vtab *pVTab,
                                            sqlite3_index_info *pIndex)
{
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "BestIndex");
#endif

    bool bMinX = false;
    bool bMinY = false;
    bool bMaxX = false;
    bool bMaxY = false;

    for (int i = 0; i < pIndex->nConstraint; i++)
    {
        int iCol = pIndex->aConstraint[i].iColumn;
        /* MinX */
        if (!bMinX && iCol == 1 && pIndex->aConstraint[i].usable &&
            (pIndex->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_LE ||
             pIndex->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_LT))
            bMinX = true;
        /* MaxX */
        else if (!bMaxX && iCol == 2 && pIndex->aConstraint[i].usable &&
                 (pIndex->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_GE ||
                  pIndex->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_GT))
            bMaxX = true;
        /* MinY */
        else if (!bMinY && iCol == 3 && pIndex->aConstraint[i].usable &&
                 (pIndex->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_LE ||
                  pIndex->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_LT))
            bMinY = true;
        /* MaxY */
        else if (!bMaxY && iCol == 4 && pIndex->aConstraint[i].usable &&
                 (pIndex->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_GE ||
                  pIndex->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_GT))
            bMaxY = true;
        else
            break;
    }

    if (bMinX && bMinY && bMaxX && bMaxY)
    {
        CPLAssert(pIndex->nConstraint == 4);

        int nConstraints = 0;
        for (int i = 0; i < pIndex->nConstraint; i++)
        {
            pIndex->aConstraintUsage[i].argvIndex = nConstraints + 1;
            pIndex->aConstraintUsage[i].omit = true;

            nConstraints++;
        }

        int *panConstraints =
            (int *)sqlite3_malloc(sizeof(int) * (1 + 2 * nConstraints));
        panConstraints[0] = nConstraints;

        nConstraints = 0;

        for (int i = 0; i < pIndex->nConstraint; i++)
        {
            if (pIndex->aConstraintUsage[i].omit)
            {
                panConstraints[2 * nConstraints + 1] =
                    pIndex->aConstraint[i].iColumn;
                panConstraints[2 * nConstraints + 2] =
                    pIndex->aConstraint[i].op;

                nConstraints++;
            }
        }

        pIndex->idxStr = (char *)panConstraints;
        pIndex->needToFreeIdxStr = true;

        pIndex->orderByConsumed = false;
        pIndex->idxNum = 0;

        return SQLITE_OK;
    }
    else
    {
        CPLDebug("OGR2SQLITE",
                 "OGR2SQLITESpatialIndex_BestIndex: unhandled request");
        return SQLITE_ERROR;
        /*
                for (i = 0; i < pIndex->nConstraint; i++)
                {
                    pIndex->aConstraintUsage[i].argvIndex = 0;
                    pIndex->aConstraintUsage[i].omit = false;
                }

                pIndex->idxStr = NULL;
                pIndex->needToFreeIdxStr = false;
        */
    }
}

/************************************************************************/
/*                  OGR2SQLITESpatialIndex_DisconnectDestroy()          */
/************************************************************************/

static int OGR2SQLITESpatialIndex_DisconnectDestroy(sqlite3_vtab *pVTab)
{
    OGR2SQLITESpatialIndex_vtab *pMyVTab = (OGR2SQLITESpatialIndex_vtab *)pVTab;

#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "DisconnectDestroy(%s)", pMyVTab->pszVTableName);
#endif

    sqlite3_free(pMyVTab->zErrMsg);
    if (pMyVTab->bCloseDS)
        delete pMyVTab->poDS;
    CPLFree(pMyVTab->pszVTableName);
    CPLFree(pMyVTab);

    return SQLITE_OK;
}

/************************************************************************/
/*                    OGR2SQLITESpatialIndex_Open()                     */
/************************************************************************/

static int OGR2SQLITESpatialIndex_Open(sqlite3_vtab *pVTab,
                                       sqlite3_vtab_cursor **ppCursor)
{
    OGR2SQLITESpatialIndex_vtab *pMyVTab = (OGR2SQLITESpatialIndex_vtab *)pVTab;
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Open(%s, %s)", pMyVTab->poDS->GetName(),
             pMyVTab->poLayer->GetName());
#endif

    GDALDataset *poDupDataSource = NULL;
    OGRLayer *poLayer = NULL;

    if (pMyVTab->nMyRef == 0)
    {
        poLayer = pMyVTab->poLayer;
    }
    else
    {
        poDupDataSource =
            (GDALDataset *)OGROpen(pMyVTab->poDS->GetName(), FALSE, NULL);
        if (poDupDataSource == NULL)
            return SQLITE_ERROR;
        poLayer = poDupDataSource->GetLayerByName(pMyVTab->poLayer->GetName());
        if (poLayer == NULL)
        {
            delete poDupDataSource;
            return SQLITE_ERROR;
        }
        if (!poLayer->GetLayerDefn()->IsSame(pMyVTab->poLayer->GetLayerDefn()))
        {
            delete poDupDataSource;
            return SQLITE_ERROR;
        }
    }
    pMyVTab->nMyRef++;

    OGR2SQLITESpatialIndex_vtab_cursor *pCursor =
        (OGR2SQLITESpatialIndex_vtab_cursor *)CPLCalloc(
            1, sizeof(OGR2SQLITESpatialIndex_vtab_cursor));
    // We do not need to fill the non-extended fields.
    *ppCursor = (sqlite3_vtab_cursor *)pCursor;

    pCursor->poDupDataSource = poDupDataSource;
    pCursor->poLayer = poLayer;
    pCursor->poLayer->ResetReading();
    pCursor->poFeature = NULL;

    return SQLITE_OK;
}

/************************************************************************/
/*                      OGR2SQLITESpatialIndex_Close()                  */
/************************************************************************/

static int OGR2SQLITESpatialIndex_Close(sqlite3_vtab_cursor *pCursor)
{
    OGR2SQLITESpatialIndex_vtab_cursor *pMyCursor =
        (OGR2SQLITESpatialIndex_vtab_cursor *)pCursor;
    OGR2SQLITESpatialIndex_vtab *pMyVTab = pMyCursor->pVTab;
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Close(%s, %s)", pMyVTab->poDS->GetName(),
             pMyVTab->poLayer->GetName());
#endif
    pMyVTab->nMyRef--;

    delete pMyCursor->poFeature;
    delete pMyCursor->poDupDataSource;

    CPLFree(pCursor);

    return SQLITE_OK;
}

/************************************************************************/
/*                     OGR2SQLITESpatialIndex_Filter()                  */
/************************************************************************/

static int OGR2SQLITESpatialIndex_Filter(sqlite3_vtab_cursor *pCursor,
                                         int idxNum, const char *idxStr,
                                         int argc, sqlite3_value **argv)
{
    OGR2SQLITESpatialIndex_vtab_cursor *pMyCursor =
        (OGR2SQLITESpatialIndex_vtab_cursor *)pCursor;
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Filter");
#endif

    int *panConstraints = (int *)idxStr;
    int nConstraints = panConstraints ? panConstraints[0] : 0;

    if (nConstraints != argc)
        return SQLITE_ERROR;

    double dfMinX = 0.0;
    double dfMaxX = 0.0;
    double dfMinY = 0.0;
    double dfMaxY = 0.0;
    for (int i = 0; i < argc; i++)
    {
        const int nCol = panConstraints[2 * i + 1];
        if (nCol < 0)
            return SQLITE_ERROR;

        double dfVal = 0.0;
        if (sqlite3_value_type(argv[i]) == SQLITE_INTEGER)
            dfVal = sqlite3_value_int64(argv[i]);
        else if (sqlite3_value_type(argv[i]) == SQLITE_FLOAT)
            dfVal = sqlite3_value_double(argv[i]);
        else
            return SQLITE_ERROR;

        if (nCol == 1)
            dfMaxX = dfVal;
        else if (nCol == 2)
            dfMinX = dfVal;
        else if (nCol == 3)
            dfMaxY = dfVal;
        else if (nCol == 4)
            dfMinY = dfVal;
        else
            return SQLITE_ERROR;
    }

#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Spatial filter : %.17g, %.17g, %.17g, %.17g",
             dfMinX, dfMinY, dfMaxX, dfMaxY);
#endif

    pMyCursor->poLayer->SetSpatialFilterRect(dfMinX, dfMinY, dfMaxX, dfMaxY);
    pMyCursor->poLayer->ResetReading();

    pMyCursor->poFeature = pMyCursor->poLayer->GetNextFeature();
    pMyCursor->bHasSetBounds = false;

    return SQLITE_OK;
}

/************************************************************************/
/*                    OGR2SQLITESpatialIndex_Next()                     */
/************************************************************************/

static int OGR2SQLITESpatialIndex_Next(sqlite3_vtab_cursor *pCursor)
{
    OGR2SQLITESpatialIndex_vtab_cursor *pMyCursor =
        (OGR2SQLITESpatialIndex_vtab_cursor *)pCursor;
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Next");
#endif

    delete pMyCursor->poFeature;
    pMyCursor->poFeature = pMyCursor->poLayer->GetNextFeature();
    pMyCursor->bHasSetBounds = false;

    return SQLITE_OK;
}

/************************************************************************/
/*                      OGR2SQLITESpatialIndex_Eof()                    */
/************************************************************************/

static int OGR2SQLITESpatialIndex_Eof(sqlite3_vtab_cursor *pCursor)
{
    OGR2SQLITESpatialIndex_vtab_cursor *pMyCursor =
        (OGR2SQLITESpatialIndex_vtab_cursor *)pCursor;
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Eof");
#endif

    return pMyCursor->poFeature == NULL;
}

/************************************************************************/
/*                    OGR2SQLITESpatialIndex_Column()                   */
/************************************************************************/

static int OGR2SQLITESpatialIndex_Column(sqlite3_vtab_cursor *pCursor,
                                         sqlite3_context *pContext, int nCol)
{
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Column %d", nCol);
#endif

    OGR2SQLITESpatialIndex_vtab_cursor *pMyCursor =
        (OGR2SQLITESpatialIndex_vtab_cursor *)pCursor;

    OGRFeature *poFeature = pMyCursor->poFeature;
    if (poFeature == NULL)
        return SQLITE_ERROR;

    if (nCol == 0)
    {
        CPLDebug("OGR2SQLITE", "--> FID = " CPL_FRMT_GIB, poFeature->GetFID());
        sqlite3_result_int64(pContext, poFeature->GetFID());
        return SQLITE_OK;
    }

    if (!pMyCursor->bHasSetBounds)
    {
        OGRGeometry *poGeom = poFeature->GetGeometryRef();
        if (poGeom != NULL && !poGeom->IsEmpty())
        {
            OGREnvelope sEnvelope;
            poGeom->getEnvelope(&sEnvelope);
            pMyCursor->bHasSetBounds = true;
            pMyCursor->dfMinX = sEnvelope.MinX;
            pMyCursor->dfMinY = sEnvelope.MinY;
            pMyCursor->dfMaxX = sEnvelope.MaxX;
            pMyCursor->dfMaxY = sEnvelope.MaxY;
        }
    }
    if (!pMyCursor->bHasSetBounds)
    {
        sqlite3_result_null(pContext);
        return SQLITE_OK;
    }

    if (nCol == 1)
    {
        sqlite3_result_double(pContext, pMyCursor->dfMinX);
        return SQLITE_OK;
    }
    if (nCol == 2)
    {
        sqlite3_result_double(pContext, pMyCursor->dfMaxX);
        return SQLITE_OK;
    }
    if (nCol == 3)
    {
        sqlite3_result_double(pContext, pMyCursor->dfMinY);
        return SQLITE_OK;
    }
    if (nCol == 4)
    {
        sqlite3_result_double(pContext, pMyCursor->dfMaxY);
        return SQLITE_OK;
    }

    return SQLITE_ERROR;
}

/************************************************************************/
/*                    OGR2SQLITESpatialIndex_Rowid()                    */
/************************************************************************/

static int OGR2SQLITESpatialIndex_Rowid(sqlite3_vtab_cursor *pCursor,
                                        sqlite3_int64 *pRowid)
{
#ifdef DEBUG_OGR2SQLITE
    CPLDebug("OGR2SQLITE", "Rowid");
#endif

    return SQLITE_ERROR;
}

/************************************************************************/
/*                   OGR2SQLITESpatialIndex_Rename()                    */
/************************************************************************/

static int OGR2SQLITESpatialIndex_Rename(sqlite3_vtab *pVtab, const char *zNew)
{
    // CPLDebug("OGR2SQLITE", "Rename");
    return SQLITE_ERROR;
}

/************************************************************************/
/*                       sOGR2SQLITESpatialIndex                        */
/************************************************************************/

static const struct sqlite3_module sOGR2SQLITESpatialIndex = {
    1,                                    /* iVersion */
    OGR2SQLITESpatialIndex_ConnectCreate, /* xCreate */
    OGR2SQLITESpatialIndex_ConnectCreate, /* xConnect */
    OGR2SQLITESpatialIndex_BestIndex,
    OGR2SQLITESpatialIndex_DisconnectDestroy, /* xDisconnect */
    OGR2SQLITESpatialIndex_DisconnectDestroy, /* xDestroy */
    OGR2SQLITESpatialIndex_Open,
    OGR2SQLITESpatialIndex_Close,
    OGR2SQLITESpatialIndex_Filter,
    OGR2SQLITESpatialIndex_Next,
    OGR2SQLITESpatialIndex_Eof,
    OGR2SQLITESpatialIndex_Column,
    OGR2SQLITESpatialIndex_Rowid,
    NULL, /* xUpdate */
    NULL, /* xBegin */
    NULL, /* xSync */
    NULL, /* xCommit */
    NULL, /* xFindFunctionRollback */
    NULL, /* xFindFunction */
    OGR2SQLITESpatialIndex_Rename};
#endif  // ENABLE_VIRTUAL_OGR_SPATIAL_INDEX

/************************************************************************/
/*                              Setup()                                 */
/************************************************************************/

int OGR2SQLITEModule::Setup(sqlite3 *hDBIn)
{
    hDB = hDBIn;

    int rc = sqlite3_create_module_v2(hDB, "VirtualOGR", &sOGR2SQLITEModule,
                                      this, OGR2SQLITEDestroyModule);
    if (rc != SQLITE_OK)
        return FALSE;

#ifdef ENABLE_VIRTUAL_OGR_SPATIAL_INDEX
    rc = sqlite3_create_module(hDB, "VirtualOGRSpatialIndex",
                               &sOGR2SQLITESpatialIndex, this);
    if (rc != SQLITE_OK)
        return FALSE;
#endif  // ENABLE_VIRTUAL_OGR_SPATIAL_INDEX

    rc = sqlite3_create_function(hDB, "ogr_layer_Extent", 1, SQLITE_ANY, this,
                                 OGR2SQLITE_ogr_layer_Extent, nullptr, nullptr);
    if (rc != SQLITE_OK)
        return FALSE;

    rc = sqlite3_create_function(hDB, "ogr_layer_SRID", 1, SQLITE_ANY, this,
                                 OGR2SQLITE_ogr_layer_SRID, nullptr, nullptr);
    if (rc != SQLITE_OK)
        return FALSE;

    rc = sqlite3_create_function(hDB, "ogr_layer_GeometryType", 1, SQLITE_ANY,
                                 this, OGR2SQLITE_ogr_layer_GeometryType,
                                 nullptr, nullptr);
    if (rc != SQLITE_OK)
        return FALSE;

    rc = sqlite3_create_function(hDB, "ogr_layer_FeatureCount", 1, SQLITE_ANY,
                                 this, OGR2SQLITE_ogr_layer_FeatureCount,
                                 nullptr, nullptr);
    if (rc != SQLITE_OK)
        return FALSE;

    SetHandleSQLFunctions(OGRSQLiteRegisterSQLFunctions(hDB));

    return TRUE;
}

/************************************************************************/
/*                        OGR2SQLITE_Setup()                            */
/************************************************************************/

OGR2SQLITEModule *OGR2SQLITE_Setup(GDALDataset *poDS,
                                   OGRSQLiteDataSource *poSQLiteDS)
{
    if (sqlite3_api == nullptr)
    {
        // Unlikely to happen. One theoretical possibility would be that:
        // - thread A calls OGR2SQLITE_Register(), which calls sqlite3_auto_extension((void (*)(void))OGR2SQLITE_static_register)
        // - thread B calls sqlite3_reset_auto_extension()
        // - thread A opens a sqlite3 handle (which normally would have caused OGR2SQLITE_static_register() to be called, and setting the sqlite3_api static variable, without prior B intervention.
        // - thread A calls us (OGR2SQLITE_Setup()) with sqlite3_api still set to its initial nullptr value
        CPLError(CE_Failure, CPLE_AppDefined,
                 "OGR2SQLITE_Setup() failed due to sqlite3_api == nullptr");
        return nullptr;
    }
    OGR2SQLITEModule *poModule = new OGR2SQLITEModule();
    poModule->Setup(poDS, poSQLiteDS);
    return poModule;
}

/************************************************************************/
/*                  OGR2SQLITE_SetCaseSensitiveLike()                   */
/************************************************************************/

void OGR2SQLITE_SetCaseSensitiveLike(OGR2SQLITEModule *poModule, bool b)
{
    poModule->SetCaseSensitiveLike(b);
}

/************************************************************************/
/*                       OGR2SQLITE_AddExtraDS()                        */
/************************************************************************/

int OGR2SQLITE_AddExtraDS(OGR2SQLITEModule *poModule, GDALDataset *poDS)
{
    return poModule->AddExtraDS(poDS);
}

#ifdef VIRTUAL_OGR_DYNAMIC_EXTENSION_ENABLED

/************************************************************************/
/*                        sqlite3_extension_init()                      */
/************************************************************************/

CPL_C_START
int CPL_DLL sqlite3_extension_init(sqlite3 *hDB, char **pzErrMsg,
                                   const sqlite3_api_routines *pApi);
CPL_C_END

/* Entry point for dynamically loaded extension (typically called by
 * load_extension()) */
int sqlite3_extension_init(sqlite3 *hDB, char **pzErrMsg,
                           const sqlite3_api_routines *pApi)
{
    CPLDebug("OGR", "OGR SQLite extension loading...");

    SQLITE_EXTENSION_INIT2(pApi);

    // Super hacky: this forces the malloc subsystem to be initialized.
    // Normally we would not need to do this, but libgdal.so links against
    // libsqlite3.so If doing SELECT load_extension('libgdal.so') from the
    // sqlite3 console binary which statically links sqlite3, we might get 2
    // copies of sqlite3 into memory: the static one from the sqlite3 binary,
    // and the shared one linked by libgdal.so If the sqlite3_create_module_v2()
    // function executed happens to be the one of the shared libsqlite3 and not
    // the one of the sqlite3 binary, then the initialization of the malloc
    // subsystem might not have been done. This demonstrates that our approach
    // of having libgdal.so to link to libsqlite3 and be a sqlite3 extension is
    // very fragile. But there aren't many other alternatives... There's no
    // problem for applications (including the sqlite3 binary) that are built
    // against a shared libsqlite3, since only one copy gets loaded.
    sqlite3_free(sqlite3_malloc(1));

    *pzErrMsg = nullptr;

    /* Check if we have been already loaded. */
    /* This is to avoid 'ogrinfo :memory: --config OGR_SQLITE_LOAD_EXTENSIONS
     * libgdal.so' to crash */
    /* since it would run OGR2SQLITEModule::Setup() first with
     * OGR2SQLITE_static_register() */
    /* and then through here. */
    int rc =
        sqlite3_exec(hDB, "SELECT ogr_version()", nullptr, nullptr, nullptr);

    /* Reset error flag */
    sqlite3_exec(hDB, "SELECT 1", nullptr, nullptr, nullptr);

    if (rc == SQLITE_OK)
    {

        CPLDebug("OGR", "... OGR virtual OGR already loaded !");
        *pzErrMsg = sqlite3_mprintf(
            "Cannot load libgdal as an extension from a OGR SQLite datasource");
        return SQLITE_ERROR;
    }

    OGRRegisterAll();

    OGR2SQLITEModule *poModule = new OGR2SQLITEModule();
    if (poModule->Setup(hDB))
    {
        CPLDebug("OGR", "OGR SQLite extension loaded");
        return SQLITE_OK;
    }
    else
        return SQLITE_ERROR;
}

#endif  // VIRTUAL_OGR_DYNAMIC_EXTENSION_ENABLED

/************************************************************************/
/*                        OGR2SQLITE_static_register()                  */
/************************************************************************/

#ifndef _WIN32
extern const struct sqlite3_api_routines OGRSQLITE_static_routines;
#endif

int OGR2SQLITE_static_register(sqlite3 *hDB, char **pzErrMsg, void *_pApi)
{
    const sqlite3_api_routines *pApi =
        static_cast<const sqlite3_api_routines *>(_pApi);
#ifndef _WIN32
    if ((pApi == nullptr) || (pApi->create_module == nullptr))
    {
        pApi = &OGRSQLITE_static_routines;
    }
#endif
    SQLITE_EXTENSION_INIT2(pApi);

    *pzErrMsg = nullptr;

    /* The config option is turned off by ogrsqliteexecutesql.cpp that needs */
    /* to create a custom module */
    if (CPLTestBool(CPLGetConfigOption("OGR_SQLITE_STATIC_VIRTUAL_OGR", "YES")))
    {
        /* Can happen if SQLite is compiled with SQLITE_OMIT_LOAD_EXTENSION
         * (with SQLite 3.6.10 for example) */
        /* We return here OK since it is not vital for regular SQLite databases
         */
        /* to load the OGR SQL functions */
        if (pApi->create_module == nullptr)
            return SQLITE_OK;

        OGR2SQLITEModule *poModule = new OGR2SQLITEModule();
        return poModule->Setup(hDB) ? SQLITE_OK : SQLITE_ERROR;
    }
    else
    {
        /* Can happen if SQLite is compiled with SQLITE_OMIT_LOAD_EXTENSION
         * (with SQLite 3.6.10 for example) */
        /* We return fail since Setup() will later be called, and crash */
        /* if create_module isn't available */
        if (pApi->create_module == nullptr)
            return SQLITE_ERROR;
    }

    return SQLITE_OK;
}

#endif  // HAVE_SQLITE3EXT_H
