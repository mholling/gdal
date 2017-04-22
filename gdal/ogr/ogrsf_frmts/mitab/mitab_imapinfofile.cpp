/**********************************************************************
 *
 * Name:     mitab_imapinfo
 * Project:  MapInfo mid/mif Tab Read/Write library
 * Language: C++
 * Purpose:  Implementation of the IMapInfoFile class, super class of
 *           of MIFFile and TABFile
 * Author:   Daniel Morissette, dmorissette@dmsolutions.ca
 *
 **********************************************************************
 * Copyright (c) 1999-2008, Daniel Morissette
 * Copyright (c) 2014, Even Rouault <even.rouault at spatialys.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 **********************************************************************/

#include "cpl_port.h"
#include "mitab.h"

#include <cctype>
#include <cstring>
#include <algorithm>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_vsi.h"
#include "mitab_priv.h"
#include "mitab_utils.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"

#ifdef __HP_aCC
#  include <wchar.h>      /* iswspace() */
#else
#  include <wctype.h>      /* iswspace() */
#endif

CPL_CVSID("$Id$");

/**********************************************************************
 *                   IMapInfoFile::IMapInfoFile()
 *
 * Constructor.
 **********************************************************************/
IMapInfoFile::IMapInfoFile() :
    m_nCurFeatureId(0),
    m_poCurFeature(NULL),
    m_bBoundsSet(FALSE),
    m_pszCharset(NULL)
{}

/**********************************************************************
 *                   IMapInfoFile::~IMapInfoFile()
 *
 * Destructor.
 **********************************************************************/
IMapInfoFile::~IMapInfoFile()
{
    if( m_poCurFeature )
    {
        delete m_poCurFeature;
        m_poCurFeature = NULL;
    }

    CPLFree(m_pszCharset);
    m_pszCharset = NULL;
}

/**********************************************************************
 *                   IMapInfoFile::Open()
 *
 * Compatibility layer with new interface.
 * Return 0 on success, -1 in case of failure.
 **********************************************************************/

int IMapInfoFile::Open(const char *pszFname, const char* pszAccess,
                       GBool bTestOpenNoError)
{
    // cppcheck-suppress nullPointer
    if( STARTS_WITH_CI(pszAccess, "r") )
        return Open(pszFname, TABRead, bTestOpenNoError);
    else if( STARTS_WITH_CI(pszAccess, "w") )
        return Open(pszFname, TABWrite, bTestOpenNoError);
    else
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Open() failed: access mode \"%s\" not supported", pszAccess);
        return -1;
    }
}

/**********************************************************************
 *                   IMapInfoFile::SmartOpen()
 *
 * Use this static method to automatically open any flavor of MapInfo
 * dataset.  This method will detect the file type, create an object
 * of the right type, and open the file.
 *
 * Call GetFileClass() on the returned object if you need to find out
 * its exact type.  (To access format-specific methods for instance)
 *
 * Returns the new object ptr. , or NULL if the open failed.
 **********************************************************************/
IMapInfoFile *IMapInfoFile::SmartOpen(const char *pszFname,
                                      GBool bUpdate,
                                      GBool bTestOpenNoError /*=FALSE*/)
{
    IMapInfoFile *poFile = NULL;
    int nLen = 0;

    if (pszFname)
        nLen = static_cast<int>(strlen(pszFname));

    if (nLen > 4 && (EQUAL(pszFname + nLen-4, ".MIF") ||
                     EQUAL(pszFname + nLen-4, ".MID") ) )
    {
        /*-------------------------------------------------------------
         * MIF/MID file
         *------------------------------------------------------------*/
        poFile = new MIFFile;
    }
    else if (nLen > 4 && EQUAL(pszFname + nLen-4, ".TAB"))
    {
        /*-------------------------------------------------------------
         * .TAB file ... is it a TABFileView or a TABFile?
         * We have to read the .tab header to find out.
         *------------------------------------------------------------*/
        char *pszAdjFname = CPLStrdup(pszFname);
        GBool bFoundFields = FALSE;
        GBool bFoundView = FALSE;
        GBool bFoundSeamless = FALSE;

        TABAdjustFilenameExtension(pszAdjFname);
        VSILFILE *fp = VSIFOpenL(pszAdjFname, "r");
        const char *pszLine = NULL;
        while(fp && (pszLine = CPLReadLineL(fp)) != NULL)
        {
            while (isspace((unsigned char)*pszLine))  pszLine++;
            if (STARTS_WITH_CI(pszLine, "Fields"))
                bFoundFields = TRUE;
            else if (STARTS_WITH_CI(pszLine, "create view"))
                bFoundView = TRUE;
            else if (STARTS_WITH_CI(pszLine, "\"\\IsSeamless\" = \"TRUE\""))
                bFoundSeamless = TRUE;
        }

        if (bFoundView)
            poFile = new TABView;
        else if (bFoundFields && bFoundSeamless)
            poFile = new TABSeamless;
        else if (bFoundFields)
            poFile = new TABFile;

        if (fp)
            VSIFCloseL(fp);

        CPLFree(pszAdjFname);
    }

    /*-----------------------------------------------------------------
     * Perform the open() call
     *----------------------------------------------------------------*/
    if (poFile && poFile->Open(pszFname, bUpdate ? TABReadWrite : TABRead, bTestOpenNoError) != 0)
    {
        delete poFile;
        poFile = NULL;
    }

    if (!bTestOpenNoError && poFile == NULL)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "%s could not be opened as a MapInfo dataset.", pszFname);
    }

    return poFile;
}

/**********************************************************************
 *                   IMapInfoFile::GetNextFeature()
 *
 * Standard OGR GetNextFeature implementation.  This method is used
 * to retrieve the next OGRFeature.
 **********************************************************************/
OGRFeature *IMapInfoFile::GetNextFeature()
{
    GIntBig nFeatureId = 0;

    while( (nFeatureId = GetNextFeatureId(m_nCurFeatureId)) != -1 )
    {
        OGRGeometry *poGeom = NULL;
        OGRFeature *poFeatureRef = GetFeatureRef(nFeatureId);
        if (poFeatureRef == NULL)
            return NULL;
        else if( (m_poFilterGeom == NULL ||
                  ((poGeom = poFeatureRef->GetGeometryRef()) != NULL &&
                   FilterGeometry( poGeom )))
                 && (m_poAttrQuery == NULL
                     || m_poAttrQuery->Evaluate( poFeatureRef )) )
        {
            // Avoid cloning feature... return the copy owned by the class
            CPLAssert(poFeatureRef == m_poCurFeature);
            m_poCurFeature = NULL;
            if( poFeatureRef->GetGeometryRef() != NULL )
                poFeatureRef->GetGeometryRef()->assignSpatialReference(GetSpatialRef());
            return poFeatureRef;
        }
    }
    return NULL;
}

/**********************************************************************
 *                   IMapInfoFile::CreateTABFeature()
 *
 * Instantiate a TABFeature* from a OGRFeature* (or NULL on error)
 **********************************************************************/

TABFeature* IMapInfoFile::CreateTABFeature(OGRFeature *poFeature)
{
    TABFeature *poTABFeature = NULL;
    OGRGeometry   *poGeom = NULL;
    OGRwkbGeometryType eGType;
    TABPoint *poTABPointFeature = NULL;
    TABRegion *poTABRegionFeature = NULL;
    TABPolyline *poTABPolylineFeature = NULL;

    /*-----------------------------------------------------------------
     * MITAB won't accept new features unless they are in a type derived
     * from TABFeature... so we have to do our best to map to the right
     * feature type based on the geometry type.
     *----------------------------------------------------------------*/
    poGeom = poFeature->GetGeometryRef();
    if( poGeom != NULL )
        eGType = poGeom->getGeometryType();
    else
        eGType = wkbNone;

    switch( wkbFlatten(eGType) )
    {
      /*-------------------------------------------------------------
       * POINT
       *------------------------------------------------------------*/
      case wkbPoint:
        poTABFeature = new TABPoint(poFeature->GetDefnRef());
        if(poFeature->GetStyleString())
        {
            poTABPointFeature = (TABPoint*)poTABFeature;
            poTABPointFeature->SetSymbolFromStyleString(
                poFeature->GetStyleString());
        }
        break;
      /*-------------------------------------------------------------
       * REGION
       *------------------------------------------------------------*/
      case wkbPolygon:
      case wkbMultiPolygon:
        poTABFeature = new TABRegion(poFeature->GetDefnRef());
        if(poFeature->GetStyleString())
        {
            poTABRegionFeature = (TABRegion*)poTABFeature;
            poTABRegionFeature->SetPenFromStyleString(
                poFeature->GetStyleString());

            poTABRegionFeature->SetBrushFromStyleString(
                poFeature->GetStyleString());
        }
        break;
      /*-------------------------------------------------------------
       * LINE/PLINE/MULTIPLINE
       *------------------------------------------------------------*/
      case wkbLineString:
      case wkbMultiLineString:
        poTABFeature = new TABPolyline(poFeature->GetDefnRef());
        if(poFeature->GetStyleString())
        {
            poTABPolylineFeature = (TABPolyline*)poTABFeature;
            poTABPolylineFeature->SetPenFromStyleString(
                poFeature->GetStyleString());
        }
        break;
      /*-------------------------------------------------------------
       * Collection types that are not directly supported... convert
       * to multiple features in output file through recursive calls.
       *------------------------------------------------------------*/
      case wkbGeometryCollection:
      case wkbMultiPoint:
      {
          OGRErr eStatus = OGRERR_NONE;
          OGRGeometryCollection *poColl = (OGRGeometryCollection*)poGeom;
          OGRFeature *poTmpFeature = poFeature->Clone();

          for( int i = 0;
               eStatus==OGRERR_NONE && i<poColl->getNumGeometries();
               i++)
          {
              poTmpFeature->SetFID(OGRNullFID);
              poTmpFeature->SetGeometry(poColl->getGeometryRef(i));
              eStatus = ICreateFeature(poTmpFeature);
          }
          delete poTmpFeature;
          return NULL;
      }
        break;
      /*-------------------------------------------------------------
       * Unsupported type.... convert to MapInfo geometry NONE
       *------------------------------------------------------------*/
      case wkbUnknown:
      default:
         poTABFeature = new TABFeature(poFeature->GetDefnRef());
        break;
    }

    if( poGeom != NULL )
        poTABFeature->SetGeometryDirectly(poGeom->clone());

    for (int i=0; i< poFeature->GetDefnRef()->GetFieldCount();i++)
    {
        poTABFeature->SetField(i,poFeature->GetRawFieldRef( i ));
    }

    poTABFeature->SetFID(poFeature->GetFID());

    return poTABFeature;
}

/**********************************************************************
 *                   IMapInfoFile::ICreateFeature()
 *
 * Standard OGR CreateFeature implementation.  This method is used
 * to create a new feature in current dataset
 **********************************************************************/
OGRErr     IMapInfoFile::ICreateFeature(OGRFeature *poFeature)
{
    TABFeature *poTABFeature = CreateTABFeature(poFeature);
    if( poTABFeature == NULL ) /* MultiGeometry */
        return OGRERR_NONE;

    OGRErr eErr = CreateFeature(poTABFeature);
    if( eErr == OGRERR_NONE )
        poFeature->SetFID(poTABFeature->GetFID());

    delete poTABFeature;

    return eErr;
}

/**********************************************************************
 *                   IMapInfoFile::GetFeature()
 *
 * Standard OGR GetFeature implementation.  This method is used
 * to get the wanted (nFeatureId) feature, a NULL value will be
 * returned on error.
 **********************************************************************/
OGRFeature *IMapInfoFile::GetFeature(GIntBig nFeatureId)
{
    /*fprintf(stderr, "GetFeature(%ld)\n", nFeatureId);*/

    OGRFeature *poFeatureRef = GetFeatureRef(nFeatureId);
    if (poFeatureRef)
    {
        // Avoid cloning feature... return the copy owned by the class
        CPLAssert(poFeatureRef == m_poCurFeature);
        m_poCurFeature = NULL;

        return poFeatureRef;
    }
    else
      return NULL;
}

/************************************************************************/
/*                            GetTABType()                              */
/*                                                                      */
/*      Create a native field based on a generic OGR definition.        */
/************************************************************************/

int IMapInfoFile::GetTABType( OGRFieldDefn *poField,
                              TABFieldType* peTABType,
                              int *pnWidth,
                              int *pnPrecision)
{
    TABFieldType        eTABType;
    int                 nWidth = poField->GetWidth();
    int                 nPrecision = poField->GetPrecision();

    if( poField->GetType() == OFTInteger )
    {
        eTABType = TABFInteger;
        if( nWidth == 0 )
            nWidth = 12;
    }
    else if( poField->GetType() == OFTReal )
    {
        if( nWidth == 0 && poField->GetPrecision() == 0)
        {
            eTABType = TABFFloat;
            nWidth = 32;
        }
        else
        {
            eTABType = TABFDecimal;
            // Enforce Mapinfo limits, otherwise MapInfo will crash (#6392)
            if( nWidth > 20 || nWidth - nPrecision < 2 || nPrecision > 16 )
            {
                if( nWidth > 20 )
                    nWidth = 20;
                if( nWidth - nPrecision < 2 )
                    nPrecision = nWidth - 2;
                if( nPrecision > 16 )
                    nPrecision = 16;
                CPLDebug( "MITAB",
                          "Adjusting initial width,precision of %s from %d,%d to %d,%d",
                          poField->GetNameRef(),
                          poField->GetWidth(), poField->GetPrecision(),
                          nWidth, nPrecision );
            }
        }
    }
    else if( poField->GetType() == OFTDate )
    {
        eTABType = TABFDate;
        if( nWidth == 0 )
            nWidth = 10;
    }
    else if( poField->GetType() == OFTTime )
    {
        eTABType = TABFTime;
        if( nWidth == 0 )
            nWidth = 9;
    }
    else if( poField->GetType() == OFTDateTime )
    {
        eTABType = TABFDateTime;
        if( nWidth == 0 )
            nWidth = 19;
    }
    else if( poField->GetType() == OFTString )
    {
        eTABType = TABFChar;
        if( nWidth == 0 )
            nWidth = 254;
        else
            nWidth = std::min(254, nWidth);
    }
    else
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "IMapInfoFile::CreateField() called with unsupported field"
                  " type %d.\n"
                  "Note that Mapinfo files don't support list field types.\n",
                  poField->GetType() );

        return -1;
    }

    *peTABType = eTABType;
    *pnWidth = nWidth;
    *pnPrecision = nPrecision;

    return 0;
}

/************************************************************************/
/*                            CreateField()                             */
/*                                                                      */
/*      Create a native field based on a generic OGR definition.        */
/************************************************************************/

OGRErr IMapInfoFile::CreateField( OGRFieldDefn *poField, int bApproxOK )

{
    TABFieldType eTABType;
    int nWidth = 0;
    int nPrecision = 0;

    if( GetTABType( poField, &eTABType, &nWidth, &nPrecision ) < 0 )
        return OGRERR_FAILURE;

    if( AddFieldNative( poField->GetNameRef(), eTABType,
                        nWidth, nPrecision, FALSE, FALSE, bApproxOK ) > -1 )
        return OGRERR_NONE;

    return OGRERR_FAILURE;
}

/**********************************************************************
 *                   IMapInfoFile::SetCharset()
 *
 * Set the charset for the tab header.
 *
 *
 * Returns 0 on success, -1 on error.
 **********************************************************************/
int IMapInfoFile::SetCharset(const char* pszCharset)
{
    if(pszCharset && strlen(pszCharset) > 0)
    {
        CPLFree(m_pszCharset);
        m_pszCharset = CPLStrdup(pszCharset);
        return 0;
    }
    return -1;
}
