/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements FileGDB OGR Datasource.
 * Author:   Ragi Yaser Burhum, ragi@burhum.com
 *           Paul Ramsey, pramsey at cleverelephant.ca
 *
 ******************************************************************************
 * Copyright (c) 2010, Ragi Yaser Burhum
 * Copyright (c) 2011, Paul Ramsey <pramsey at cleverelephant.ca>
 * Copyright (c) 2011-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_fgdb.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "gdal.h"
#include "FGdbUtils.h"
#include "cpl_multiproc.h"

#include "filegdb_fielddomain.h"
#include "filegdb_relationship.h"

using std::vector;
using std::wstring;

/***********************************************************************/
/*                         OGRFileGDBGroup                             */
/***********************************************************************/

class OGRFileGDBGroup final : public GDALGroup
{
  protected:
    friend class FGdbDataSource;
    std::vector<std::shared_ptr<GDALGroup>> m_apoSubGroups{};
    std::vector<OGRLayer *> m_apoLayers{};

  public:
    OGRFileGDBGroup(const std::string &osParentName, const char *pszName)
        : GDALGroup(osParentName, pszName)
    {
    }

    std::vector<std::string>
    GetGroupNames(CSLConstList papszOptions) const override;
    std::shared_ptr<GDALGroup>
    OpenGroup(const std::string &osName,
              CSLConstList papszOptions) const override;

    std::vector<std::string>
    GetVectorLayerNames(CSLConstList papszOptions) const override;
    OGRLayer *OpenVectorLayer(const std::string &osName,
                              CSLConstList papszOptions) const override;
};

std::vector<std::string> OGRFileGDBGroup::GetGroupNames(CSLConstList) const
{
    std::vector<std::string> ret;
    for (const auto &poSubGroup : m_apoSubGroups)
        ret.emplace_back(poSubGroup->GetName());
    return ret;
}

std::shared_ptr<GDALGroup> OGRFileGDBGroup::OpenGroup(const std::string &osName,
                                                      CSLConstList) const
{
    for (const auto &poSubGroup : m_apoSubGroups)
    {
        if (poSubGroup->GetName() == osName)
            return poSubGroup;
    }
    return nullptr;
}

std::vector<std::string>
OGRFileGDBGroup::GetVectorLayerNames(CSLConstList) const
{
    std::vector<std::string> ret;
    for (const auto &poLayer : m_apoLayers)
        ret.emplace_back(poLayer->GetName());
    return ret;
}

OGRLayer *OGRFileGDBGroup::OpenVectorLayer(const std::string &osName,
                                           CSLConstList) const
{
    for (const auto &poLayer : m_apoLayers)
    {
        if (poLayer->GetName() == osName)
            return poLayer;
    }
    return nullptr;
}

/************************************************************************/
/*                          FGdbDataSource()                           */
/************************************************************************/

FGdbDataSource::FGdbDataSource(bool bUseDriverMutex,
                               FGdbDatabaseConnection *pConnection,
                               bool bUseOpenFileGDB)
    : m_bUseDriverMutex(bUseDriverMutex), m_pConnection(pConnection),
      m_pGeodatabase(nullptr), m_poOpenFileGDBDrv(nullptr),
      m_bUseOpenFileGDB(bUseOpenFileGDB)
{
    SetDescription(pConnection->m_osName.c_str());
    poDriver = GDALDriver::FromHandle(GDALGetDriverByName("FileGDB"));
}

/************************************************************************/
/*                          ~FGdbDataSource()                          */
/************************************************************************/

FGdbDataSource::~FGdbDataSource()
{
    CPLMutexHolderOptionalLockD(m_bUseDriverMutex ? FGdbDriver::hMutex
                                                  : nullptr);

    // CloseInternal();
    size_t count = m_layers.size();
    for (size_t i = 0; i < count; ++i)
    {
        if (FGdbLayer *poFgdbLayer = dynamic_cast<FGdbLayer *>(m_layers[i]))
        {
            poFgdbLayer->CloseGDBObjects();
        }
    }

    if (m_bUseDriverMutex)
        FGdbDriver::Release(m_osPublicName);

    // size_t count = m_layers.size();
    for (size_t i = 0; i < count; ++i)
    {
        // only delete layers coming from this driver -- the tables opened by
        // the OpenFileGDB driver will be deleted when we close the OpenFileGDB
        // datasource
        if (FGdbLayer *poFgdbLayer = dynamic_cast<FGdbLayer *>(m_layers[i]))
        {
            delete poFgdbLayer;
        }
    }
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int FGdbDataSource::Open(const char *pszNewName, int /* bUpdate */,
                         const char *pszPublicName)
{
    m_osFSName = pszNewName;
    m_osPublicName = (pszPublicName) ? pszPublicName : pszNewName;
    m_pGeodatabase = m_pConnection->GetGDB();
    m_poOpenFileGDBDrv = (GDALDriver *)GDALGetDriverByName("OpenFileGDB");

    return LoadLayers(L"\\");
}

/************************************************************************/
/*                           CloseInternal()                            */
/************************************************************************/

int FGdbDataSource::CloseInternal(int bCloseGeodatabase)
{
    size_t count = m_layers.size();
    for (size_t i = 0; i < count; ++i)
    {
        if (FGdbLayer *poFgdbLayer = dynamic_cast<FGdbLayer *>(m_layers[i]))
        {
            poFgdbLayer->CloseGDBObjects();
        }
    }

    if (m_pConnection && bCloseGeodatabase)
        m_pConnection->CloseGeodatabase();
    m_pGeodatabase = nullptr;
    return true;
}

/************************************************************************/
/*                          OpenFGDBTables()                            */
/************************************************************************/

bool FGdbDataSource::OpenFGDBTables(OGRFileGDBGroup *group,
                                    const std::wstring &type,
                                    const std::vector<std::wstring> &layers)
{
#ifdef DISPLAY_RELATED_DATASETS
    std::vector<std::wstring> relationshipTypes;
    m_pGeodatabase->GetDatasetRelationshipTypes(relationshipTypes);
    std::vector<std::wstring> datasetTypes;
    m_pGeodatabase->GetDatasetTypes(datasetTypes);
#endif

    fgdbError hr;
    for (unsigned int i = 0; i < layers.size(); i++)
    {
        Table *pTable = new Table;
        // CPLDebug("FGDB", "Opening %s", WStringToString(layers[i]).c_str());
        if (FAILED(hr = m_pGeodatabase->OpenTable(layers[i], *pTable)))
        {
            delete pTable;

            std::wstring fgdb_error_desc_w;
            fgdbError er;
            er = FileGDBAPI::ErrorInfo::GetErrorDescription(hr,
                                                            fgdb_error_desc_w);
            const char *pszLikelyReason =
                "Might be due to unsupported spatial reference system. Using "
                "OpenFileGDB driver or FileGDB SDK >= 1.4 should solve it";
            if (er == S_OK)
            {
                std::string fgdb_error_desc =
                    WStringToString(fgdb_error_desc_w);
                if (fgdb_error_desc == "FileGDB compression is not installed.")
                {
                    pszLikelyReason = "Using FileGDB SDK 1.4 or later should "
                                      "solve this issue.";
                }
            }

            GDBErr(hr, "Error opening " + WStringToString(layers[i]),
                   CE_Warning,
                   (". Skipping it. " + CPLString(pszLikelyReason)).c_str());
            continue;
        }

#ifdef DISPLAY_RELATED_DATASETS
        CPLDebug("FGDB", "Finding datasets related to %s",
                 WStringToString(layers[i]).c_str());
        // Slow !
        for (const auto &relType : relationshipTypes)
        {
            for (const auto &itemType : datasetTypes)
            {
                std::vector<std::wstring> relatedDatasets;
                m_pGeodatabase->GetRelatedDatasets(layers[i], relType, itemType,
                                                   relatedDatasets);

                std::vector<std::string> relatedDatasetDefs;
                m_pGeodatabase->GetRelatedDatasetDefinitions(
                    layers[i], relType, itemType, relatedDatasetDefs);

                if (!relatedDatasets.empty() || !relatedDatasetDefs.empty())
                {
                    CPLDebug("FGDB", "relationshipType: %s",
                             WStringToString(relType).c_str());
                    CPLDebug("FGDB", "itemType: %s",
                             WStringToString(itemType).c_str());
                }
                for (const auto &name : relatedDatasets)
                {
                    CPLDebug("FGDB", "Related dataset: %s",
                             WStringToString(name).c_str());
                }
                for (const auto &xml : relatedDatasetDefs)
                {
                    CPLDebug("FGDB", "Related dataset def: %s", xml.c_str());
                }
            }
        }
#endif

        FGdbLayer *pLayer = new FGdbLayer();
        if (!pLayer->Initialize(this, pTable, layers[i], type))
        {
            delete pLayer;
            return GDBErr(hr, "Error initializing OGRLayer for " +
                                  WStringToString(layers[i]));
        }

        m_layers.push_back(pLayer);
        group->m_apoLayers.emplace_back(pLayer);
    }
    return true;
}

/************************************************************************/
/*                            LoadLayers()                             */
/************************************************************************/

bool FGdbDataSource::LoadLayers(const std::wstring &root)
{
    std::vector<wstring> tables;
    std::vector<wstring> featureclasses;
    std::vector<wstring> featuredatasets;
    fgdbError hr;

    auto poRootGroup = std::make_shared<OGRFileGDBGroup>(std::string(), "");
    m_poRootGroup = poRootGroup;

    /* Find all the Tables in the root */
    if (FAILED(hr = m_pGeodatabase->GetChildDatasets(root, L"Table", tables)))
    {
        return GDBErr(hr, "Error reading Tables in " + WStringToString(root));
    }
    /* Open the tables we found */
    if (!tables.empty() && !OpenFGDBTables(poRootGroup.get(), L"Table", tables))
        return false;

    /* Find all the Feature Classes in the root */
    if (FAILED(hr = m_pGeodatabase->GetChildDatasets(root, L"Feature Class",
                                                     featureclasses)))
    {
        return GDBErr(hr, "Error reading Feature Classes in " +
                              WStringToString(root));
    }
    /* Open the tables we found */
    if (!featureclasses.empty() &&
        !OpenFGDBTables(poRootGroup.get(), L"Feature Class", featureclasses))
        return false;

    /* Find all the Feature Datasets in the root */
    if (FAILED(hr = m_pGeodatabase->GetChildDatasets(root, L"Feature Dataset",
                                                     featuredatasets)))
    {
        return GDBErr(hr, "Error reading Feature Datasets in " +
                              WStringToString(root));
    }
    /* Look for Feature Classes inside the Feature Dataset */
    for (unsigned int i = 0; i < featuredatasets.size(); i++)
    {
        const std::string featureDatasetPath(
            WStringToString(featuredatasets[i]));
        if (FAILED(hr = m_pGeodatabase->GetChildDatasets(
                       featuredatasets[i], L"Feature Class", featureclasses)))
        {
            return GDBErr(hr, "Error reading Feature Classes in " +
                                  featureDatasetPath);
        }
        std::string featureDatasetName(featureDatasetPath);
        if (!featureDatasetName.empty() && featureDatasetPath[0] == '\\')
            featureDatasetName = featureDatasetName.substr(1);
        auto poFeatureDatasetGroup = std::make_shared<OGRFileGDBGroup>(
            poRootGroup->GetName(), featureDatasetName.c_str());
        poRootGroup->m_apoSubGroups.emplace_back(poFeatureDatasetGroup);
        if (!featureclasses.empty() &&
            !OpenFGDBTables(poFeatureDatasetGroup.get(), L"Feature Class",
                            featureclasses))
            return false;
    }

    // Look for items which aren't present in the GDB_Items table (see
    // https://github.com/OSGeo/gdal/issues/4463) We do this by browsing the
    // catalog table (using the OpenFileGDB driver) and looking for items we
    // haven't yet found. If we find any, we have no choice but to load these
    // using the OpenFileGDB driver, as the ESRI SDK refuses to acknowledge that
    // they exist (despite ArcGIS itself showing them!)
    int iGDBItems = -1;
    const char *const apszDrivers[2] = {"OpenFileGDB", nullptr};
    const std::string osSystemCatalog =
        CPLFormFilenameSafe(m_osFSName, "a00000001", "gdbtable");
    if (m_bUseOpenFileGDB)
    {
        m_poOpenFileGDBDS.reset(GDALDataset::Open(osSystemCatalog.c_str(),
                                                  GDAL_OF_VECTOR, apszDrivers,
                                                  nullptr, nullptr));
    }
    if (m_poOpenFileGDBDS != nullptr &&
        m_poOpenFileGDBDS->GetLayer(0) != nullptr)
    {
        OGRLayer *poCatalogLayer = m_poOpenFileGDBDS->GetLayer(0);
        const int iNameIndex =
            poCatalogLayer->GetLayerDefn()->GetFieldIndex("Name");
        int i = -1;
        for (auto &poFeat : poCatalogLayer)
        {
            i++;
            const std::string osTableName =
                poFeat->GetFieldAsString(iNameIndex);

            if (osTableName.compare("GDB_Items") == 0)
            {
                iGDBItems = i;
            }

            // test if layer is already added
            if (GDALDataset::GetLayerByName(osTableName.c_str()))
                continue;

            const CPLString osLCTableName(CPLString(osTableName).tolower());
            const bool bIsPrivateLayer = osLCTableName.size() >= 4 &&
                                         osLCTableName.substr(0, 4) == "gdb_";
            if (!bIsPrivateLayer)
            {
                if (OGRLayer *poLayer =
                        m_poOpenFileGDBDS->GetLayerByName(osTableName.c_str()))
                {
                    m_layers.push_back(poLayer);
                    poRootGroup->m_apoLayers.emplace_back(poLayer);
                }
            }
        }
    }

    // Read relationships. Note that we don't use the SDK method for this, as
    // it's too slow!
    if (iGDBItems >= 0)
    {
        const std::string osGDBItems = CPLFormFilenameSafe(
            m_osFSName, CPLSPrintf("a%08x", iGDBItems + 1), "gdbtable");
        std::unique_ptr<GDALDataset> poGDBItems(GDALDataset::Open(
            osGDBItems.c_str(), GDAL_OF_VECTOR, apszDrivers, nullptr, nullptr));
        if (poGDBItems != nullptr && poGDBItems->GetLayer(0) != nullptr)
        {
            if (OGRLayer *poItemsLayer = poGDBItems->GetLayer(0))
            {
                const int iType = poItemsLayer->FindFieldIndex("Type", true);
                const int iDefinition =
                    poItemsLayer->FindFieldIndex("Definition", true);
                if (iType < 0 || iDefinition < 0)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Wrong structure for GDB_Items table");
                    return FALSE;
                }

                // Hunt for relationships in GDB_Items table
                for (const auto &poFeature : poItemsLayer)
                {
                    CPLString osType;
                    const char *pszType = poFeature->GetFieldAsString(iType);
                    if (pszType != nullptr &&
                        EQUAL(pszType, pszRelationshipTypeUUID))
                    {
                        // relationship item
                        auto poRelationship = ParseXMLRelationshipDef(
                            poFeature->GetFieldAsString(iDefinition));
                        if (poRelationship)
                        {
                            const std::string relationshipName(
                                poRelationship->GetName());
                            m_osMapRelationships[relationshipName] =
                                std::move(poRelationship);
                        }
                    }
                }
            }
        }
    }

    return true;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int FGdbDataSource::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, ODsCMeasuredGeometries))
        return TRUE;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *FGdbDataSource::GetLayer(int iLayer)
{
    int count = static_cast<int>(m_layers.size());

    if (iLayer < 0 || iLayer >= count)
        return nullptr;
    else
        return m_layers[iLayer];
}

/************************************************************************/
/*                   OGRFGdbSingleFeatureLayer                          */
/************************************************************************/

class OGRFGdbSingleFeatureLayer final : public OGRLayer
{
  private:
    char *pszVal;
    OGRFeatureDefn *poFeatureDefn;
    int iNextShapeId;

  public:
    OGRFGdbSingleFeatureLayer(const char *pszLayerName, const char *pszVal);
    virtual ~OGRFGdbSingleFeatureLayer();

    virtual void ResetReading() override
    {
        iNextShapeId = 0;
    }

    virtual OGRFeature *GetNextFeature() override;

    virtual OGRFeatureDefn *GetLayerDefn() override
    {
        return poFeatureDefn;
    }

    virtual int TestCapability(const char *) override
    {
        return FALSE;
    }
};

/************************************************************************/
/*                    OGRFGdbSingleFeatureLayer()                       */
/************************************************************************/

OGRFGdbSingleFeatureLayer::OGRFGdbSingleFeatureLayer(const char *pszLayerName,
                                                     const char *pszValIn)
{
    poFeatureDefn = new OGRFeatureDefn(pszLayerName);
    SetDescription(poFeatureDefn->GetName());
    poFeatureDefn->Reference();
    OGRFieldDefn oField("FIELD_1", OFTString);
    poFeatureDefn->AddFieldDefn(&oField);

    iNextShapeId = 0;
    pszVal = pszValIn ? CPLStrdup(pszValIn) : nullptr;
}

/************************************************************************/
/*                   ~OGRFGdbSingleFeatureLayer()                       */
/************************************************************************/

OGRFGdbSingleFeatureLayer::~OGRFGdbSingleFeatureLayer()
{
    if (poFeatureDefn != nullptr)
        poFeatureDefn->Release();
    CPLFree(pszVal);
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRFGdbSingleFeatureLayer::GetNextFeature()
{
    if (iNextShapeId != 0)
        return nullptr;

    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    if (pszVal)
        poFeature->SetField(0, pszVal);
    poFeature->SetFID(iNextShapeId++);
    return poFeature;
}

/************************************************************************/
/*                              ExecuteSQL()                            */
/************************************************************************/

OGRLayer *FGdbDataSource::ExecuteSQL(const char *pszSQLCommand,
                                     OGRGeometry *poSpatialFilter,
                                     const char *pszDialect)

{
    if (m_pGeodatabase == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Use generic implementation for recognized dialects              */
    /* -------------------------------------------------------------------- */
    if (IsGenericSQLDialect(pszDialect))
        return GDALDataset::ExecuteSQL(pszSQLCommand, poSpatialFilter,
                                       pszDialect);

    /* -------------------------------------------------------------------- */
    /*      Special case GetLayerDefinition                                 */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszSQLCommand, "GetLayerDefinition "))
    {
        FGdbLayer *poLayer = cpl::down_cast<FGdbLayer *>(
            GetLayerByName(pszSQLCommand + strlen("GetLayerDefinition ")));
        if (poLayer)
        {
            char *pszVal = nullptr;
            poLayer->GetLayerXML(&pszVal);
            OGRLayer *poRet =
                new OGRFGdbSingleFeatureLayer("LayerDefinition", pszVal);
            CPLFree(pszVal);
            return poRet;
        }
        else
            return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Special case GetLayerMetadata                                   */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszSQLCommand, "GetLayerMetadata "))
    {
        FGdbLayer *poLayer = cpl::down_cast<FGdbLayer *>(
            GetLayerByName(pszSQLCommand + strlen("GetLayerMetadata ")));
        if (poLayer)
        {
            char *pszVal = nullptr;
            poLayer->GetLayerMetadataXML(&pszVal);
            OGRLayer *poRet =
                new OGRFGdbSingleFeatureLayer("LayerMetadata", pszVal);
            CPLFree(pszVal);
            return poRet;
        }
        else
            return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Special case REPACK                                             */
    /* -------------------------------------------------------------------- */
    if (EQUAL(pszSQLCommand, "REPACK"))
    {
        auto hr = m_pGeodatabase->CompactDatabase();
        if (FAILED(hr))
        {
            GDBErr(hr, "CompactDatabase failed");
            return new OGRFGdbSingleFeatureLayer("result", "false");
        }
        return new OGRFGdbSingleFeatureLayer("result", "true");
    }

    /* TODO: remove that workaround when the SDK has finally a decent */
    /* SQL support ! */
    if (STARTS_WITH_CI(pszSQLCommand, "SELECT ") && pszDialect == nullptr)
    {
        CPLDebug("FGDB", "Support for SELECT is known to be partially "
                         "non-compliant with FileGDB SDK API v1.2.\n"
                         "So for now, we use default OGR SQL engine. "
                         "Explicitly specify -dialect FileGDB\n"
                         "to use the SQL engine from the FileGDB SDK API");
        OGRLayer *poLayer =
            GDALDataset::ExecuteSQL(pszSQLCommand, poSpatialFilter, pszDialect);
        if (poLayer)
            m_oSetSelectLayers.insert(poLayer);
        return poLayer;
    }

    /* -------------------------------------------------------------------- */
    /*      Run the SQL                                                     */
    /* -------------------------------------------------------------------- */
    EnumRows *pEnumRows = new EnumRows;
    long hr;
    try
    {
        hr = m_pGeodatabase->ExecuteSQL(StringToWString(pszSQLCommand), true,
                                        *pEnumRows);
    }
    catch (...)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Exception occurred at executing '%s'. Application may "
                 "become unstable",
                 pszSQLCommand);
        delete pEnumRows;
        return nullptr;
    }

    if (FAILED(hr))
    {
        GDBErr(hr, CPLSPrintf("Failed at executing '%s'", pszSQLCommand));
        delete pEnumRows;
        return nullptr;
    }

    if (STARTS_WITH_CI(pszSQLCommand, "SELECT "))
    {
        OGRLayer *poLayer = new FGdbResultLayer(this, pszSQLCommand, pEnumRows);
        m_oSetSelectLayers.insert(poLayer);
        return poLayer;
    }
    else
    {
        delete pEnumRows;
        return nullptr;
    }
}

/************************************************************************/
/*                           ReleaseResultSet()                         */
/************************************************************************/

void FGdbDataSource::ReleaseResultSet(OGRLayer *poResultsSet)
{
    if (poResultsSet)
        m_oSetSelectLayers.erase(poResultsSet);
    delete poResultsSet;
}

/************************************************************************/
/*                        GetFieldDomain()                              */
/************************************************************************/

const OGRFieldDomain *
FGdbDataSource::GetFieldDomain(const std::string &name) const
{
    const auto baseRet = GDALDataset::GetFieldDomain(name);
    if (baseRet)
        return baseRet;

    std::string domainDef;
    const auto hr =
        m_pGeodatabase->GetDomainDefinition(StringToWString(name), domainDef);
    if (FAILED(hr))
    {
        // GDBErr(hr, "Failed in GetDomainDefinition()");
        return nullptr;
    }

    auto poDomain = ParseXMLFieldDomainDef(domainDef);
    if (!poDomain)
        return nullptr;
    const std::string domainName(poDomain->GetName());
    m_oMapFieldDomains[domainName] = std::move(poDomain);
    return GDALDataset::GetFieldDomain(name);
}

/************************************************************************/
/*                        GetFieldDomainNames()                         */
/************************************************************************/

std::vector<std::string> FGdbDataSource::GetFieldDomainNames(CSLConstList) const
{
    std::vector<std::wstring> oDomainNamesWList;
    const auto hr = m_pGeodatabase->GetDomains(oDomainNamesWList);
    if (FAILED(hr))
    {
        GDBErr(hr, "Failed in GetDomains()");
        return std::vector<std::string>();
    }

    std::vector<std::string> oDomainNamesList;
    oDomainNamesList.reserve(oDomainNamesWList.size());
    for (const auto &osNameW : oDomainNamesWList)
    {
        oDomainNamesList.emplace_back(WStringToString(osNameW));
    }
    return oDomainNamesList;
}

/************************************************************************/
/*                        GetRelationshipNames()                        */
/************************************************************************/

std::vector<std::string>
FGdbDataSource::GetRelationshipNames(CPL_UNUSED CSLConstList papszOptions) const

{
    std::vector<std::string> oasNames;
    oasNames.reserve(m_osMapRelationships.size());
    for (auto it = m_osMapRelationships.begin();
         it != m_osMapRelationships.end(); ++it)
    {
        oasNames.emplace_back(it->first);
    }
    return oasNames;
}

/************************************************************************/
/*                        GetRelationship()                             */
/************************************************************************/

const GDALRelationship *
FGdbDataSource::GetRelationship(const std::string &name) const

{
    auto it = m_osMapRelationships.find(name);
    if (it == m_osMapRelationships.end())
        return nullptr;

    return it->second.get();
}
