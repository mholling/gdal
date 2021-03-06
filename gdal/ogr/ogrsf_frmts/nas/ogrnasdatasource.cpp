/******************************************************************************
 *
 * Project:  OGR
 * Purpose:  Implements OGRNASDataSource class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2010-2013, Even Rouault <even dot rouault at mines-paris dot org>
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogr_nas.h"

CPL_CVSID("$Id$");

static const char * const apszURNNames[] =
{
    "DE_DHDN_3GK2_*", "EPSG:31466",
    "DE_DHDN_3GK3_*", "EPSG:31467",
    "ETRS89_UTM32", "EPSG:25832",
    "ETRS89_UTM33", "EPSG:25833",
    NULL, NULL
};

/************************************************************************/
/*                         OGRNASDataSource()                           */
/************************************************************************/

OGRNASDataSource::OGRNASDataSource() :
    papoLayers(NULL),
    nLayers(0),
    poRelationLayer(NULL),
    pszName(NULL),
    poReader(NULL)
{}

/************************************************************************/
/*                        ~OGRNASDataSource()                         */
/************************************************************************/

OGRNASDataSource::~OGRNASDataSource()

{
    CPLFree( pszName );

    for( int i = 0; i < nLayers; i++ )
        delete papoLayers[i];

    CPLFree( papoLayers );

    if( poReader )
        delete poReader;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRNASDataSource::Open( const char * pszNewName )

{
    poReader = CreateNASReader();
    if( poReader == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "File %s appears to be NAS but the NAS reader cannot\n"
                  "be instantiated, likely because Xerces support was not\n"
                  "configured in.",
                  pszNewName );
        return FALSE;
    }

    poReader->SetSourceFile( pszNewName );

    pszName = CPLStrdup( pszNewName );

/* -------------------------------------------------------------------- */
/*      Can we find a NAS Feature Schema (.gfs) for the input file?     */
/* -------------------------------------------------------------------- */
    bool bHaveSchema = false;

    const char *pszGFSFilename = CPLResetExtension( pszNewName, "gfs" );
    VSIStatBufL sGFSStatBuf;
    if( VSIStatL( pszGFSFilename, &sGFSStatBuf ) == 0 )
    {
        VSIStatBufL sNASStatBuf;
        if( VSIStatL( pszNewName, &sNASStatBuf ) == 0 &&
            sNASStatBuf.st_mtime > sGFSStatBuf.st_mtime )
        {
            CPLDebug( "NAS",
                      "Found %s but ignoring because it appears\n"
                      "be older than the associated NAS file.",
                      pszGFSFilename );
        }
        else
        {
            bHaveSchema = poReader->LoadClasses( pszGFSFilename );
        }
    }

/* -------------------------------------------------------------------- */
/*      Force a first pass to establish the schema.  Eventually we      */
/*      will have mechanisms for remembering the schema and related     */
/*      information.                                                    */
/* -------------------------------------------------------------------- */
    CPLErrorReset();
    if( !bHaveSchema
        && !poReader->PrescanForSchema( TRUE )
        && CPLGetLastErrorType() == CE_Failure )
    {
        // Assume an error has been reported.
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Save the schema file if possible.  Do not make a fuss if we     */
/*      cannot.  It could be read-only directory or something.          */
/* -------------------------------------------------------------------- */
    if( !bHaveSchema && poReader->GetClassCount() > 0 )
    {
        VSILFILE *fp = NULL;

        pszGFSFilename = CPLResetExtension( pszNewName, "gfs" );
        if( VSIStatL( pszGFSFilename, &sGFSStatBuf ) != 0
            && (fp = VSIFOpenL( pszGFSFilename, "wt" )) != NULL )
        {
            VSIFCloseL( fp );
            poReader->SaveClasses( pszGFSFilename );
        }
        else
        {
            CPLDebug( "NAS",
                      "Not saving %s files already exists or can't be created.",
                      pszGFSFilename );
        }
    }

/* -------------------------------------------------------------------- */
/*      Translate the NASFeatureClasses into layers.                    */
/* -------------------------------------------------------------------- */
    papoLayers = (OGRLayer **)
        CPLCalloc( sizeof(OGRNASLayer *), poReader->GetClassCount()+1 );
    nLayers = 0;

    while( nLayers < poReader->GetClassCount() )
    {
        papoLayers[nLayers] = TranslateNASSchema(poReader->GetClass(nLayers));
        nLayers++;
    }

    poRelationLayer = new OGRNASRelationLayer( this );

    // keep delete the last layer
    if( nLayers>0 && EQUAL( papoLayers[nLayers-1]->GetName(), "Delete" ) )
    {
      papoLayers[nLayers]   = papoLayers[nLayers-1];
      papoLayers[nLayers-1] = poRelationLayer;
    }
    else
    {
      papoLayers[nLayers] = poRelationLayer;
    }

    nLayers++;

    return TRUE;
}

/************************************************************************/
/*                         TranslateNASSchema()                         */
/************************************************************************/

OGRNASLayer *OGRNASDataSource::TranslateNASSchema( GMLFeatureClass *poClass )

{
    OGRwkbGeometryType eGType = wkbNone;

    if( poClass->GetGeometryPropertyCount() != 0 )
    {
        eGType = static_cast<OGRwkbGeometryType>(
            poClass->GetGeometryProperty(0)->GetType() );

        if( poClass->GetFeatureCount() == 0 )
            eGType = wkbUnknown;
    }

/* -------------------------------------------------------------------- */
/*      Translate SRS.                                                  */
/* -------------------------------------------------------------------- */
    const char* pszSRSName = poClass->GetSRSName();
    OGRSpatialReference* poSRS = NULL;
    if (pszSRSName)
    {
        const char *pszHandle = strrchr( pszSRSName, ':' );
        if( pszHandle != NULL )
        {
            pszHandle += 1;

            poSRS = new OGRSpatialReference();

            for( int i = 0; apszURNNames[i*2+0] != NULL; i++ )
            {
                const char *pszTarget = apszURNNames[i*2+0];
                const int nTLen = static_cast<int>(strlen(pszTarget));

                // Are we just looking for a prefix match?
                if( pszTarget[nTLen-1] == '*' )
                {
                    if( EQUALN(pszTarget,pszHandle,nTLen-1) )
                        pszSRSName = apszURNNames[i*2+1];
                }
                else
                {
                    if( EQUAL(pszTarget,pszHandle) )
                        pszSRSName = apszURNNames[i*2+1];
                }
            }

            if (poSRS->SetFromUserInput(pszSRSName) != OGRERR_NONE)
            {
                CPLDebug( "NAS", "Failed to translate srsName='%s'",
                        pszSRSName );
                delete poSRS;
                poSRS = NULL;
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Create an empty layer.                                          */
/* -------------------------------------------------------------------- */
    OGRNASLayer *poLayer =
        new OGRNASLayer( poClass->GetName(), poSRS, eGType, this );
    delete poSRS;

/* -------------------------------------------------------------------- */
/*      Added attributes (properties).                                  */
/* -------------------------------------------------------------------- */
    for( int iField = 0; iField < poClass->GetPropertyCount(); iField++ )
    {
        GMLPropertyDefn *poProperty = poClass->GetProperty( iField );
        OGRFieldType eFType;

        if( poProperty->GetType() == GMLPT_Untyped )
            eFType = OFTString;
        else if( poProperty->GetType() == GMLPT_String )
            eFType = OFTString;
        else if( poProperty->GetType() == GMLPT_Integer )
            eFType = OFTInteger;
        else if( poProperty->GetType() == GMLPT_Real )
            eFType = OFTReal;
        else if( poProperty->GetType() == GMLPT_StringList )
            eFType = OFTStringList;
        else if( poProperty->GetType() == GMLPT_IntegerList )
            eFType = OFTIntegerList;
        else if( poProperty->GetType() == GMLPT_RealList )
            eFType = OFTRealList;
        else
            eFType = OFTString;

        OGRFieldDefn oField( poProperty->GetName(), eFType );
        if ( STARTS_WITH_CI(oField.GetNameRef(), "ogr:") )
          oField.SetName(poProperty->GetName()+4);
        if( poProperty->GetWidth() > 0 )
            oField.SetWidth( poProperty->GetWidth() );

        poLayer->GetLayerDefn()->AddFieldDefn( &oField );
    }

    return poLayer;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRNASDataSource::GetLayer( int iLayer )

{
    if( iLayer < 0 || iLayer >= nLayers )
        return NULL;

    return papoLayers[iLayer];
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRNASDataSource::TestCapability( const char * /* pszCap */ )
{
    return FALSE;
}

/************************************************************************/
/*                         PopulateRelations()                          */
/************************************************************************/

void OGRNASDataSource::PopulateRelations()

{
    poReader->ResetReading();

    GMLFeature  *poFeature = NULL;
    while( (poFeature = poReader->NextFeature()) != NULL )
    {
        char **papszOBProperties = poFeature->GetOBProperties();

        for( int i = 0;
             papszOBProperties != NULL && papszOBProperties[i] != NULL;
             i++ )
        {
            const int nGMLIdIndex =
                poFeature->GetClass()->GetPropertyIndex( "gml_id" );
            const GMLProperty *psGMLId =
              (nGMLIdIndex >= 0) ? poFeature->GetProperty(nGMLIdIndex ) : NULL;
            char *l_pszName = NULL;
            const char *pszValue = CPLParseNameValue( papszOBProperties[i],
                                                      &l_pszName );

            if( STARTS_WITH_CI(pszValue, "urn:adv:oid:")
                && psGMLId != NULL && psGMLId->nSubProperties == 1 )
            {
                poRelationLayer->AddRelation( psGMLId->papszSubProperties[0],
                                              l_pszName,
                                              pszValue + 12 );
            }
            CPLFree( l_pszName );
        }

        delete poFeature;
    }

    poRelationLayer->MarkRelationsPopulated();
}
