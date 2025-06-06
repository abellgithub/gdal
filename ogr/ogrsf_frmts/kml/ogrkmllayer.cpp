/******************************************************************************
 *
 * Project:  KML Driver
 * Purpose:  Implementation of OGRKMLLayer class.
 * Author:   Christopher Condit, condit@sdsc.edu
 *           Jens Oberender, j.obi@troja.net
 *
 ******************************************************************************
 * Copyright (c) 2006, Christopher Condit
 * Copyright (c) 2007-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "ogr_kml.h"

#include <string>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "kml.h"
#include "kmlutility.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_featurestyle.h"
#include "ogr_geometry.h"
#include "ogr_p.h"
#include "ogr_spatialref.h"
#include "ogrsf_frmts.h"

/* Function utility to dump OGRGeometry to KML text. */
char *OGR_G_ExportToKML(OGRGeometryH hGeometry, const char *pszAltitudeMode);

/************************************************************************/
/*                           OGRKMLLayer()                              */
/************************************************************************/

OGRKMLLayer::OGRKMLLayer(const char *pszName,
                         const OGRSpatialReference *poSRSIn, bool bWriterIn,
                         OGRwkbGeometryType eReqType, OGRKMLDataSource *poDSIn)
    : poDS_(poDSIn),
      poSRS_(poSRSIn ? new OGRSpatialReference(nullptr) : nullptr),
      poFeatureDefn_(new OGRFeatureDefn(pszName)), bWriter_(bWriterIn),
      pszName_(CPLStrdup(pszName))
{
    // KML should be created as WGS84.
    if (poSRSIn != nullptr)
    {
        poSRS_->SetWellKnownGeogCS("WGS84");
        poSRS_->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (!poSRS_->IsSame(poSRSIn))
        {
            poCT_ = OGRCreateCoordinateTransformation(poSRSIn, poSRS_);
            if (poCT_ == nullptr && poDSIn->IsFirstCTError())
            {
                // If we can't create a transformation, issue a warning - but
                // continue the transformation.
                char *pszWKT = nullptr;

                poSRSIn->exportToPrettyWkt(&pszWKT, FALSE);

                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "Failed to create coordinate transformation between the "
                    "input coordinate system and WGS84.  This may be because "
                    "they are not transformable.  "
                    "KML geometries may not render correctly.  "
                    "This message will not be issued any more."
                    "\nSource:\n%s\n",
                    pszWKT);

                CPLFree(pszWKT);
                poDSIn->IssuedFirstCTError();
            }
        }
    }

    SetDescription(poFeatureDefn_->GetName());
    poFeatureDefn_->Reference();
    poFeatureDefn_->SetGeomType(eReqType);
    if (poFeatureDefn_->GetGeomFieldCount() != 0)
        poFeatureDefn_->GetGeomFieldDefn(0)->SetSpatialRef(poSRS_);

    OGRFieldDefn oFieldName("Name", OFTString);
    poFeatureDefn_->AddFieldDefn(&oFieldName);

    OGRFieldDefn oFieldDesc("Description", OFTString);
    poFeatureDefn_->AddFieldDefn(&oFieldDesc);

    bClosedForWriting = !bWriterIn;
}

/************************************************************************/
/*                           ~OGRKMLLayer()                             */
/************************************************************************/

OGRKMLLayer::~OGRKMLLayer()
{
    if (nullptr != poFeatureDefn_)
        poFeatureDefn_->Release();

    if (nullptr != poSRS_)
        poSRS_->Release();

    if (nullptr != poCT_)
        delete poCT_;

    CPLFree(pszName_);
}

/************************************************************************/
/*                            GetLayerDefn()                            */
/************************************************************************/

OGRFeatureDefn *OGRKMLLayer::GetLayerDefn()
{
    return poFeatureDefn_;
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGRKMLLayer::ResetReading()
{
    iNextKMLId_ = 0;
    nLastAsked = -1;
    nLastCount = -1;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRKMLLayer::GetNextFeature()
{
#ifndef HAVE_EXPAT
    return nullptr;
#else
    /* -------------------------------------------------------------------- */
    /*      Loop till we find a feature matching our criteria.              */
    /* -------------------------------------------------------------------- */
    KML *poKMLFile = poDS_->GetKMLFile();
    if (poKMLFile == nullptr)
        return nullptr;

    poKMLFile->selectLayer(nLayerNumber_);

    while (true)
    {
        auto poFeatureKML = std::unique_ptr<Feature>(
            poKMLFile->getFeature(iNextKMLId_++, nLastAsked, nLastCount));

        if (poFeatureKML == nullptr)
            return nullptr;

        auto poFeature = std::make_unique<OGRFeature>(poFeatureDefn_);

        if (poFeatureKML->poGeom)
        {
            poFeature->SetGeometry(std::move(poFeatureKML->poGeom));
        }

        // Add fields.
        poFeature->SetField(poFeatureDefn_->GetFieldIndex("Name"),
                            poFeatureKML->sName.c_str());
        poFeature->SetField(poFeatureDefn_->GetFieldIndex("Description"),
                            poFeatureKML->sDescription.c_str());
        poFeature->SetFID(iNextKMLId_ - 1);

        if (poFeature->GetGeometryRef() != nullptr && poSRS_ != nullptr)
        {
            poFeature->GetGeometryRef()->assignSpatialReference(poSRS_);
        }

        // Check spatial/attribute filters.
        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeometryRef())) &&
            (m_poAttrQuery == nullptr ||
             m_poAttrQuery->Evaluate(poFeature.get())))
        {
            return poFeature.release();
        }
    }

#endif /* HAVE_EXPAT */
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/************************************************************************/

#ifndef HAVE_EXPAT
GIntBig OGRKMLLayer::GetFeatureCount(int /* bForce */)
{
    return 0;
}
#else

GIntBig OGRKMLLayer::GetFeatureCount(int bForce)
{
    if (m_poFilterGeom != nullptr || m_poAttrQuery != nullptr)
        return OGRLayer::GetFeatureCount(bForce);

    KML *poKMLFile = poDS_->GetKMLFile();
    if (nullptr == poKMLFile)
        return 0;

    poKMLFile->selectLayer(nLayerNumber_);

    return poKMLFile->getNumFeatures();
}
#endif

/************************************************************************/
/*                           WriteSchema()                              */
/************************************************************************/

CPLString OGRKMLLayer::WriteSchema()
{
    if (bSchemaWritten_)
        return "";

    CPLString osRet;

    OGRFeatureDefn *featureDefinition = GetLayerDefn();
    for (int j = 0; j < featureDefinition->GetFieldCount(); j++)
    {
        OGRFieldDefn *fieldDefinition = featureDefinition->GetFieldDefn(j);

        if (nullptr != poDS_->GetNameField() &&
            EQUAL(fieldDefinition->GetNameRef(), poDS_->GetNameField()))
            continue;

        if (nullptr != poDS_->GetDescriptionField() &&
            EQUAL(fieldDefinition->GetNameRef(), poDS_->GetDescriptionField()))
            continue;

        if (osRet.empty())
        {
            osRet += CPLSPrintf("<Schema name=\"%s\" id=\"%s\">\n", pszName_,
                                pszName_);
        }

        const char *pszKMLType = nullptr;
        const char *pszKMLEltName = nullptr;
        // Match the OGR type to the GDAL type.
        switch (fieldDefinition->GetType())
        {
            case OFTInteger:
                pszKMLType = "int";
                pszKMLEltName = "SimpleField";
                break;
            case OFTIntegerList:
                pszKMLType = "int";
                pszKMLEltName = "SimpleArrayField";
                break;
            case OFTReal:
                pszKMLType = "float";
                pszKMLEltName = "SimpleField";
                break;
            case OFTRealList:
                pszKMLType = "float";
                pszKMLEltName = "SimpleArrayField";
                break;
            case OFTString:
                pszKMLType = "string";
                pszKMLEltName = "SimpleField";
                break;
            case OFTStringList:
                pszKMLType = "string";
                pszKMLEltName = "SimpleArrayField";
                break;
                // TODO: KML doesn't handle these data types yet...
            case OFTDate:
            case OFTTime:
            case OFTDateTime:
                pszKMLType = "string";
                pszKMLEltName = "SimpleField";
                break;

            default:
                pszKMLType = "string";
                pszKMLEltName = "SimpleField";
                break;
        }
        osRet += CPLSPrintf("\t<%s name=\"%s\" type=\"%s\"></%s>\n",
                            pszKMLEltName, fieldDefinition->GetNameRef(),
                            pszKMLType, pszKMLEltName);
    }

    if (!osRet.empty())
        osRet += CPLSPrintf("%s", "</Schema>\n");

    return osRet;
}

/************************************************************************/
/*                           ICreateFeature()                            */
/************************************************************************/

OGRErr OGRKMLLayer::ICreateFeature(OGRFeature *poFeature)
{
    CPLAssert(nullptr != poFeature);
    CPLAssert(nullptr != poDS_);

    if (!bWriter_)
        return OGRERR_FAILURE;

    if (bClosedForWriting)
    {
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "Interleaved feature adding to different layers is not supported");
        return OGRERR_FAILURE;
    }

    VSILFILE *fp = poDS_->GetOutputFP();
    CPLAssert(nullptr != fp);

    if (poDS_->GetLayerCount() == 1 && nWroteFeatureCount_ == 0)
    {
        CPLString osRet = WriteSchema();
        if (!osRet.empty())
            VSIFPrintfL(fp, "%s", osRet.c_str());
        bSchemaWritten_ = true;

        VSIFPrintfL(fp, "<Folder><name>%s</name>\n", pszName_);
    }

    ++nWroteFeatureCount_;
    char *pszEscapedLayerName = OGRGetXML_UTF8_EscapedString(GetDescription());
    VSIFPrintfL(fp, "  <Placemark id=\"%s." CPL_FRMT_GIB "\">\n",
                pszEscapedLayerName, nWroteFeatureCount_);
    CPLFree(pszEscapedLayerName);

    if (poFeature->GetFID() == OGRNullFID)
        poFeature->SetFID(iNextKMLId_++);

    // Find and write the name element
    if (nullptr != poDS_->GetNameField())
    {
        for (int iField = 0; iField < poFeatureDefn_->GetFieldCount(); iField++)
        {
            OGRFieldDefn *poField = poFeatureDefn_->GetFieldDefn(iField);

            if (poFeature->IsFieldSetAndNotNull(iField) &&
                EQUAL(poField->GetNameRef(), poDS_->GetNameField()))
            {
                const char *pszRaw = poFeature->GetFieldAsString(iField);
                while (*pszRaw == ' ')
                    pszRaw++;

                char *pszEscaped = OGRGetXML_UTF8_EscapedString(pszRaw);

                VSIFPrintfL(fp, "\t<name>%s</name>\n", pszEscaped);
                CPLFree(pszEscaped);
            }
        }
    }

    if (nullptr != poDS_->GetDescriptionField())
    {
        for (int iField = 0; iField < poFeatureDefn_->GetFieldCount(); iField++)
        {
            OGRFieldDefn *poField = poFeatureDefn_->GetFieldDefn(iField);

            if (poFeature->IsFieldSetAndNotNull(iField) &&
                EQUAL(poField->GetNameRef(), poDS_->GetDescriptionField()))
            {
                const char *pszRaw = poFeature->GetFieldAsString(iField);
                while (*pszRaw == ' ')
                    pszRaw++;

                char *pszEscaped = OGRGetXML_UTF8_EscapedString(pszRaw);

                VSIFPrintfL(fp, "\t<description>%s</description>\n",
                            pszEscaped);
                CPLFree(pszEscaped);
            }
        }
    }

    OGRwkbGeometryType eGeomType = wkbNone;
    if (poFeature->GetGeometryRef() != nullptr)
        eGeomType = wkbFlatten(poFeature->GetGeometryRef()->getGeometryType());

    if (wkbPolygon == eGeomType || wkbMultiPolygon == eGeomType ||
        wkbLineString == eGeomType || wkbMultiLineString == eGeomType)
    {
        OGRStylePen *poPen = nullptr;
        OGRStyleMgr oSM;

        if (poFeature->GetStyleString() != nullptr)
        {
            oSM.InitFromFeature(poFeature);

            for (int i = 0; i < oSM.GetPartCount(); i++)
            {
                OGRStyleTool *poTool = oSM.GetPart(i);
                if (poTool && poTool->GetType() == OGRSTCPen)
                {
                    poPen = cpl::down_cast<OGRStylePen *>(poTool);
                    break;
                }
                delete poTool;
            }
        }

        VSIFPrintfL(fp, "\t<Style>");
        if (poPen != nullptr)
        {
            GBool bDefault = FALSE;

            /* Require width to be returned in pixel */
            poPen->SetUnit(OGRSTUPixel);
            double fW = poPen->Width(bDefault);
            if (bDefault)
                fW = 1;
            const char *pszColor = poPen->Color(bDefault);
            const int nColorLen = static_cast<int>(CPLStrnlen(pszColor, 10));
            if (pszColor != nullptr && pszColor[0] == '#' && !bDefault &&
                nColorLen >= 7)
            {
                char acColor[9] = {0};
                /* Order of KML color is aabbggrr, whereas OGR color is
                 * #rrggbb[aa] ! */
                if (nColorLen == 9)
                {
                    acColor[0] = pszColor[7]; /* A */
                    acColor[1] = pszColor[8];
                }
                else
                {
                    acColor[0] = 'F';
                    acColor[1] = 'F';
                }
                acColor[2] = pszColor[5]; /* B */
                acColor[3] = pszColor[6];
                acColor[4] = pszColor[3]; /* G */
                acColor[5] = pszColor[4];
                acColor[6] = pszColor[1]; /* R */
                acColor[7] = pszColor[2];
                VSIFPrintfL(fp, "<LineStyle><color>%s</color>", acColor);
                VSIFPrintfL(fp, "<width>%g</width>", fW);
                VSIFPrintfL(fp, "</LineStyle>");
            }
            else
            {
                VSIFPrintfL(fp,
                            "<LineStyle><color>ff0000ff</color></LineStyle>");
            }
        }
        else
        {
            VSIFPrintfL(fp, "<LineStyle><color>ff0000ff</color></LineStyle>");
        }
        delete poPen;
        // If we're dealing with a polygon, add a line style that will stand out
        // a bit.
        VSIFPrintfL(fp, "<PolyStyle><fill>0</fill></PolyStyle></Style>\n");
    }

    bool bHasFoundOtherField = false;

    // Write all fields as SchemaData
    for (int iField = 0; iField < poFeatureDefn_->GetFieldCount(); iField++)
    {
        OGRFieldDefn *poField = poFeatureDefn_->GetFieldDefn(iField);

        if (poFeature->IsFieldSetAndNotNull(iField))
        {
            if (nullptr != poDS_->GetNameField() &&
                EQUAL(poField->GetNameRef(), poDS_->GetNameField()))
                continue;

            if (nullptr != poDS_->GetDescriptionField() &&
                EQUAL(poField->GetNameRef(), poDS_->GetDescriptionField()))
                continue;

            if (!bHasFoundOtherField)
            {
                VSIFPrintfL(fp,
                            "\t<ExtendedData><SchemaData schemaUrl=\"#%s\">\n",
                            pszName_);
                bHasFoundOtherField = true;
            }
            const char *pszRaw = poFeature->GetFieldAsString(iField);

            while (*pszRaw == ' ')
                pszRaw++;

            char *pszEscaped = nullptr;
            if (poFeatureDefn_->GetFieldDefn(iField)->GetType() == OFTReal)
            {
                pszEscaped = CPLStrdup(pszRaw);
            }
            else
            {
                pszEscaped = OGRGetXML_UTF8_EscapedString(pszRaw);
            }

            VSIFPrintfL(fp, "\t\t<SimpleData name=\"%s\">%s</SimpleData>\n",
                        poField->GetNameRef(), pszEscaped);

            CPLFree(pszEscaped);
        }
    }

    if (bHasFoundOtherField)
    {
        VSIFPrintfL(fp, "\t</SchemaData></ExtendedData>\n");
    }

    // Write out Geometry - for now it isn't indented properly.
    if (poFeature->GetGeometryRef() != nullptr)
    {
        char *pszGeometry = nullptr;
        OGREnvelope sGeomBounds;
        OGRGeometry *poWGS84Geom = nullptr;

        if (nullptr != poCT_)
        {
            poWGS84Geom = poFeature->GetGeometryRef()->clone();
            poWGS84Geom->transform(poCT_);
        }
        else
        {
            poWGS84Geom = poFeature->GetGeometryRef();
        }

        pszGeometry = OGR_G_ExportToKML(OGRGeometry::ToHandle(poWGS84Geom),
                                        poDS_->GetAltitudeMode());
        if (pszGeometry != nullptr)
        {
            VSIFPrintfL(fp, "      %s\n", pszGeometry);
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Export of geometry to KML failed");
        }
        CPLFree(pszGeometry);

        poWGS84Geom->getEnvelope(&sGeomBounds);
        poDS_->GrowExtents(&sGeomBounds);

        if (nullptr != poCT_)
        {
            delete poWGS84Geom;
        }
    }

    VSIFPrintfL(fp, "  </Placemark>\n");
    return OGRERR_NONE;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRKMLLayer::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, OLCSequentialWrite))
    {
        return bWriter_;
    }
    else if (EQUAL(pszCap, OLCCreateField))
    {
        return bWriter_ && iNextKMLId_ == 0;
    }
    else if (EQUAL(pszCap, OLCFastFeatureCount))
    {
        //        if( poFClass == NULL
        //            || m_poFilterGeom != NULL
        //            || m_poAttrQuery != NULL )
        return FALSE;

        //        return poFClass->GetFeatureCount() != -1;
    }

    else if (EQUAL(pszCap, OLCStringsAsUTF8))
        return TRUE;
    else if (EQUAL(pszCap, OLCZGeometries))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                            CreateField()                             */
/************************************************************************/

OGRErr OGRKMLLayer::CreateField(const OGRFieldDefn *poField,
                                CPL_UNUSED int bApproxOK)
{
    if (!bWriter_ || iNextKMLId_ != 0)
        return OGRERR_FAILURE;

    OGRFieldDefn oCleanCopy(poField);
    poFeatureDefn_->AddFieldDefn(&oCleanCopy);

    return OGRERR_NONE;
}

/************************************************************************/
/*                           SetLayerNumber()                           */
/************************************************************************/

void OGRKMLLayer::SetLayerNumber(int nLayer)
{
    nLayerNumber_ = nLayer;
}

/************************************************************************/
/*                             GetDataset()                             */
/************************************************************************/

GDALDataset *OGRKMLLayer::GetDataset()
{
    return poDS_;
}
