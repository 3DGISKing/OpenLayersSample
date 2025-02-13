/***************************************************************************
                          qgscoordinatereferencesystem.cpp

                             -------------------
    begin                : 2007
    copyright            : (C) 2007 by Gary E. Sherman
    email                : sherman@mrcc.com
***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "qgscoordinatereferencesystem.h"
#include "qgscoordinatereferencesystem_p.h"

#include <cmath>

#include <QDir>
#include <QTemporaryFile>
#include <QtXml/QDomNode>
#include <QtXml/QDomElement>
#include <QFileInfo>
#include <QRegExp>
#include <QTextStream>
#include <QFile>
#include <QSettings>

#include <QApplication>

//#include "qgsapplication.h"
//#include "qgscrscache.h"
//#include "qgslogger.h"
//#include "qgsmessagelog.h"
//#include "qgis.h" //const vals declared here
#include "qgslocalec.h"

#include <sqlite3.h>
#include <proj_api.h>

//gdal and ogr includes (needed for == operator)
#include <ogr_srs_api.h>
#include <cpl_error.h>
#include <cpl_conv.h>
#include <cpl_csv.h>

CUSTOM_CRS_VALIDATION QgsCoordinateReferenceSystem::mCustomSrsValidation = 0;

//--------------------------

#define QgsDebugMsg qDebug
#define nullptr NULL	

QString qgisUserDbFilePath;
const int USER_CRS_START_ID = 100000;
const int LAT_PREFIX_LEN = 7;
const long GEOCRS_ID = 4326;

//! Returns a string representation of a double
//! @param a double value
//! @param precision number of decimal places to retain
inline QString qgsDoubleToString( double a, int precision = 17 )
{
	if ( precision )
		return QString::number( a, 'f', precision ).remove( QRegExp( "\\.?0+$" ) );
	else
		return QString::number( a, 'f', precision );
}

//! Compare two doubles (but allow some difference)
//! @param a first double
//! @param b second double
//! @param epsilon maximum difference allowable between doubles
inline bool qgsDoubleNear( double a, double b, double epsilon = 4 * DBL_EPSILON )
{
	const double diff = a - b;
	return diff > -epsilon && diff <= epsilon;
}

QgsCoordinateReferenceSystem::QgsCoordinateReferenceSystem()
{
  srsDbFilePath = QApplication::applicationDirPath() + "/srs.db";

  d = new QgsCoordinateReferenceSystemPrivate();
}

QgsCoordinateReferenceSystem::QgsCoordinateReferenceSystem( const QString& theDefinition )
{
  srsDbFilePath = QApplication::applicationDirPath() + "/srs.db";

  d = new QgsCoordinateReferenceSystemPrivate();
  createFromString( theDefinition );
}

QgsCoordinateReferenceSystem::QgsCoordinateReferenceSystem( const long theId, CrsType theType )
{
   srsDbFilePath = QApplication::applicationDirPath() + "/srs.db";
  
  d = new QgsCoordinateReferenceSystemPrivate();
  createFromId( theId, theType );
}

QgsCoordinateReferenceSystem::QgsCoordinateReferenceSystem( const QgsCoordinateReferenceSystem &srs )
    : d( srs.d )
{
	 srsDbFilePath = QApplication::applicationDirPath() + "/srs.db";
}

QgsCoordinateReferenceSystem& QgsCoordinateReferenceSystem::operator=( const QgsCoordinateReferenceSystem & srs )
{
	 srsDbFilePath = QApplication::applicationDirPath() + "/srs.db";
  d = srs.d;
  return *this;
}

QgsCoordinateReferenceSystem::~QgsCoordinateReferenceSystem()
{
}

bool QgsCoordinateReferenceSystem::createFromId( const long theId, CrsType theType )
{
  bool result = false;
  switch ( theType )
  {
    case InternalCrsId:
      result = createFromSrsId( theId );
      break;
    case PostgisCrsId:
      result = createFromSrid( theId );
      break;
    case EpsgCrsId:
      result = createFromOgcWmsCrs( QString( "EPSG:%1" ).arg( theId ) );
      break;
    default:
      //THIS IS BAD...THIS PART OF CODE SHOULD NEVER BE REACHED...
      QgsDebugMsg( "Unexpected case reached!" );
  };
  return result;
}

bool QgsCoordinateReferenceSystem::createFromString( const QString &theDefinition )
{
  bool result = false;
  QRegExp reCrsId( "^(epsg|postgis|internal)\\:(\\d+)$", Qt::CaseInsensitive );
  if ( reCrsId.indexIn( theDefinition ) == 0 )
  {
    QString authName = reCrsId.cap( 1 ).toLower();
    CrsType type = InternalCrsId;
    if ( authName == "epsg" )
      type = EpsgCrsId;
    if ( authName == "postgis" )
      type = PostgisCrsId;
    long id = reCrsId.cap( 2 ).toLong();
    result = createFromId( id, type );
  }
  else
  {
    QRegExp reCrsStr( "^(?:(wkt|proj4)\\:)?(.+)$", Qt::CaseInsensitive );
    if ( reCrsStr.indexIn( theDefinition ) == 0 )
    {
      if ( reCrsStr.cap( 1 ).toLower() == "proj4" )
      {
        result = createFromProj4( reCrsStr.cap( 2 ) );
        //TODO: createFromProj4 used to save to the user database any new CRS
        // this behavior was changed in order to separate creation and saving.
        // Not sure if it necessary to save it here, should be checked by someone
        // familiar with the code (should also give a more descriptive name to the generated CRS)
        if ( srsid() == 0 )
        {
          QString myName = QString( " * %1 (%2)" )
                           .arg( QObject::tr( "Generated CRS", "A CRS automatically generated from layer info get this prefix for description" ),
                                 toProj4() );
          saveAsUserCRS( myName );
        }
      }
      else
      {
        result = createFromWkt( reCrsStr.cap( 2 ) );
      }
    }
  }
  return result;
}

bool QgsCoordinateReferenceSystem::createFromUserInput( const QString &theDefinition )
{
  QString theWkt;
  char *wkt = nullptr;
  OGRSpatialReferenceH crs = OSRNewSpatialReference( nullptr );

  // make sure towgs84 parameter is loaded if using an ESRI definition and gdal >= 1.9
#if GDAL_VERSION_NUM >= 1900
  if ( theDefinition.startsWith( "ESRI::" ) )
  {
    setupESRIWktFix();
  }
#endif

  if ( OSRSetFromUserInput( crs, theDefinition.toLocal8Bit().constData() ) == OGRERR_NONE )
  {
    if ( OSRExportToWkt( crs, &wkt ) == OGRERR_NONE )
    {
      theWkt = wkt;
      OGRFree( wkt );
    }
    OSRDestroySpatialReference( crs );
  }
  //QgsDebugMsg( "theDefinition: " + theDefinition + " theWkt = " + theWkt );
  return createFromWkt( theWkt );
}

void QgsCoordinateReferenceSystem::setupESRIWktFix()
{
  // make sure towgs84 parameter is loaded if gdal >= 1.9
  // this requires setting GDAL_FIX_ESRI_WKT=GEOGCS (see qgis bug #5598 and gdal bug #4673)
#if GDAL_VERSION_NUM >= 1900
  const char* configOld = CPLGetConfigOption( "GDAL_FIX_ESRI_WKT", "" );
  const char* configNew = "GEOGCS";
  // only set if it was not set, to let user change the value if needed
  if ( strcmp( configOld, "" ) == 0 )
  {
    CPLSetConfigOption( "GDAL_FIX_ESRI_WKT", configNew );
   /* if ( strcmp( configNew, CPLGetConfigOption( "GDAL_FIX_ESRI_WKT", "" ) ) != 0 )
      QgsLogger::warning( QString( "GDAL_FIX_ESRI_WKT could not be set to %1 : %2" )
                          .arg( configNew, CPLGetConfigOption( "GDAL_FIX_ESRI_WKT", "" ) ) );
    QgsDebugMsg( QString( "set GDAL_FIX_ESRI_WKT : %1" ).arg( configNew ) );*/
  }
  else
  {
    //QgsDebugMsg( QString( "GDAL_FIX_ESRI_WKT was already set : %1" ).arg( configNew ) );
  }
#endif
}

bool QgsCoordinateReferenceSystem::createFromOgcWmsCrs( const QString& theCrs )
{
  QString wmsCrs = theCrs;

  QRegExp re( "urn:ogc:def:crs:([^:]+).+([^:]+)", Qt::CaseInsensitive );
  if ( re.exactMatch( wmsCrs ) )
  {
    wmsCrs = re.cap( 1 ) + ':' + re.cap( 2 );
  }
  else
  {
    re.setPattern( "(user|custom|qgis):(\\d+)" );
    if ( re.exactMatch( wmsCrs ) && createFromSrsId( re.cap( 2 ).toInt() ) )
    {
      return true;
    }
  }

  if ( loadFromDb( srsDbFilePath, "lower(auth_name||':'||auth_id)", wmsCrs.toLower() ) )
    return true;

  // NAD27
  if ( wmsCrs.compare( "CRS:27", Qt::CaseInsensitive ) == 0 ||
       wmsCrs.compare( "OGC:CRS27", Qt::CaseInsensitive ) == 0 )
  {
    // TODO: verify same axis orientation
    return createFromOgcWmsCrs( "EPSG:4267" );
  }

  // NAD83
  if ( wmsCrs.compare( "CRS:83", Qt::CaseInsensitive ) == 0 ||
       wmsCrs.compare( "OGC:CRS83", Qt::CaseInsensitive ) == 0 )
  {
    // TODO: verify same axis orientation
    return createFromOgcWmsCrs( "EPSG:4269" );
  }

  // WGS84
  if ( wmsCrs.compare( "CRS:84", Qt::CaseInsensitive ) == 0 ||
       wmsCrs.compare( "OGC:CRS84", Qt::CaseInsensitive ) == 0 )
  {
    createFromOgcWmsCrs( "EPSG:4326" );

    d.detach();
    d->mAxisInverted = false;
    d->mAxisInvertedDirty = false;

    return d->mIsValid;
  }

  return false;
}

// Misc helper functions -----------------------


void QgsCoordinateReferenceSystem::validate()
{
  if ( d->mIsValid )
    return;

  d.detach();

  // try to validate using custom validation routines
  if ( mCustomSrsValidation )
    mCustomSrsValidation( *this );

  /*if ( !d->mIsValid )
  {
    *this = QgsCRSCache::instance()->crsByOgcWmsCrs( GEO_EPSG_CRS_AUTHID );
  }*/
}

bool QgsCoordinateReferenceSystem::createFromSrid( long id )
{
  return loadFromDb( srsDbFilePath, "srid", QString::number( id ) );
}

bool QgsCoordinateReferenceSystem::createFromSrsId( long id )
{
  return loadFromDb( id < USER_CRS_START_ID ? srsDbFilePath :
                     qgisUserDbFilePath,
                     "srs_id", QString::number( id ) );
}

bool QgsCoordinateReferenceSystem::loadFromDb( const QString& db, const QString& expression, const QString& value )
{
  d.detach();

 // QgsDebugMsgLevel( "load CRS from " + db + " where " + expression + " is " + value, 3 );
  d->mIsValid = false;
  d->mWkt.clear();

  QFileInfo myInfo( db );
  if ( !myInfo.exists() )
  {
   // QgsDebugMsg( "failed : " + db + " does not exist!" );
    return d->mIsValid;
  }

  sqlite3      *myDatabase;
  const char   *myTail;
  sqlite3_stmt *myPreparedStatement;
  int           myResult;
  //check the db is available
  myResult = openDb( db, &myDatabase );
  if ( myResult != SQLITE_OK )
  {
//    QgsDebugMsg( "failed : " + db + " could not be opened!" );
    return d->mIsValid;
  }

  /*
    srs_id INTEGER PRIMARY KEY,
    description text NOT NULL,
    projection_acronym text NOT NULL,
    ellipsoid_acronym NOT NULL,
    parameters text NOT NULL,
    srid integer NOT NULL,
    auth_name varchar NOT NULL,
    auth_id integer NOT NULL,
    is_geo integer NOT NULL);
  */

  QString mySql = "select srs_id,description,projection_acronym,"
                  "ellipsoid_acronym,parameters,srid,auth_name||':'||auth_id,is_geo "
                  "from tbl_srs where " + expression + '=' + quotedValue( value ) + " order by deprecated";
  myResult = sqlite3_prepare( myDatabase, mySql.toUtf8(),
                              mySql.toUtf8().length(),
                              &myPreparedStatement, &myTail );
  // XXX Need to free memory from the error msg if one is set
  if ( myResult == SQLITE_OK && sqlite3_step( myPreparedStatement ) == SQLITE_ROW )
  {
    d->mSrsId = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text(
                                     myPreparedStatement, 0 ) ) ).toLong();
    d->mDescription = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text(
                                           myPreparedStatement, 1 ) ) );
    d->mProjectionAcronym = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 2 ) ) );
    d->mEllipsoidAcronym = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 3 ) ) );
    d->mProj4 = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 4 ) ) );
    d->mSRID = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 5 ) ) ).toLong() ;
    d->mAuthId = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 6 ) ) );
    d->mIsGeographic = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 7 ) ) ).toInt() != 0;
    d->mAxisInvertedDirty = true;

    if ( d->mSrsId >= USER_CRS_START_ID && d->mAuthId.isEmpty() )
    {
      d->mAuthId = QString( "USER:%1" ).arg( d->mSrsId );
    }
    else if ( d->mAuthId.startsWith( "EPSG:", Qt::CaseInsensitive ) )
    {
      OSRDestroySpatialReference( d->mCRS );
      d->mCRS = OSRNewSpatialReference( nullptr );
      d->mIsValid = OSRSetFromUserInput( d->mCRS, d->mAuthId.toLower().toAscii() ) == OGRERR_NONE;
      setMapUnits();
    }

    if ( !d->mIsValid )
    {
      setProj4String( d->mProj4 );
    }
  }
  else
  {
//    QgsDebugMsg( "failed : " + mySql );
  }
  sqlite3_finalize( myPreparedStatement );
  sqlite3_close( myDatabase );
  return d->mIsValid;
}

bool QgsCoordinateReferenceSystem::axisInverted() const
{
  if ( d->mAxisInvertedDirty )
  {
    OGRAxisOrientation orientation;
    OSRGetAxis( d->mCRS, OSRIsGeographic( d->mCRS ) ? "GEOGCS" : "PROJCS", 0, &orientation );

    // If axis orientation is unknown, try again with OSRImportFromEPSGA for EPSG crs
    if ( orientation == OAO_Other && d->mAuthId.startsWith( "EPSG:", Qt::CaseInsensitive ) )
    {
      OGRSpatialReferenceH crs = OSRNewSpatialReference( nullptr );

      if ( OSRImportFromEPSGA( crs, d->mAuthId.mid( 5 ).toInt() ) == OGRERR_NONE )
      {
        OSRGetAxis( crs, OSRIsGeographic( crs ) ? "GEOGCS" : "PROJCS", 0, &orientation );
      }

      OSRDestroySpatialReference( crs );
    }

    d->mAxisInverted = orientation == OAO_North;
    d->mAxisInvertedDirty = false;
  }

  return d->mAxisInverted;
}

bool QgsCoordinateReferenceSystem::createFromWkt( const QString &theWkt )
{
  d.detach();

  d->mIsValid = false;
  d->mWkt.clear();
  d->mProj4.clear();

  if ( theWkt.isEmpty() )
  {
    QgsDebugMsg( "theWkt is uninitialized, operation failed" );
    return d->mIsValid;
  }
//  QgsDebugMsg( "wkt: " + theWkt );
  QByteArray ba = theWkt.toLatin1();
  const char *pWkt = ba.data();

  OGRErr myInputResult = OSRImportFromWkt( d->mCRS, const_cast< char ** >( & pWkt ) );

  if ( myInputResult != OGRERR_NONE )
  {
    /*QgsDebugMsg( "\n---------------------------------------------------------------" );
    QgsDebugMsg( "This CRS could *** NOT *** be set from the supplied Wkt " );
    QgsDebugMsg( "INPUT: " + theWkt );
    QgsDebugMsg( QString( "UNUSED WKT: %1" ).arg( pWkt ) );
    QgsDebugMsg( "---------------------------------------------------------------\n" );*/
    return d->mIsValid;
  }

  if ( OSRAutoIdentifyEPSG( d->mCRS ) == OGRERR_NONE )
  {
    QString authid = QString( "%1:%2" )
                     .arg( OSRGetAuthorityName( d->mCRS, nullptr ),
                           OSRGetAuthorityCode( d->mCRS, nullptr ) );
//    QgsDebugMsg( "authid recognized as " + authid );
    return createFromOgcWmsCrs( authid );
  }

  // always morph from esri as it doesn't hurt anything
  // FW: Hey, that's not right!  It can screw stuff up! Disable
  //myOgrSpatialRef.morphFromESRI();

  // create the proj4 structs needed for transforming
  char *proj4src = nullptr;
  OSRExportToProj4( d->mCRS, &proj4src );

  //now that we have the proj4string, delegate to createFromProj4 so
  // that we can try to fill in the remaining class members...
  //create from Proj will set the isValidFlag
  if ( !createFromProj4( proj4src ) )
  {
    CPLFree( proj4src );

    // try fixed up version
    OSRFixup( d->mCRS );

    OSRExportToProj4( d->mCRS, &proj4src );

    createFromProj4( proj4src );
  }
  //TODO: createFromProj4 used to save to the user database any new CRS
  // this behavior was changed in order to separate creation and saving.
  // Not sure if it necessary to save it here, should be checked by someone
  // familiar with the code (should also give a more descriptive name to the generated CRS)
  if ( d->mSrsId == 0 )
  {
    QString myName = QString( " * %1 (%2)" )
                     .arg( QObject::tr( "Generated CRS", "A CRS automatically generated from layer info get this prefix for description" ),
                           toProj4() );
    saveAsUserCRS( myName );
  }

  CPLFree( proj4src );

  return d->mIsValid;
  //setMapunits will be called by createfromproj above
}

bool QgsCoordinateReferenceSystem::isValid() const
{
  return d->mIsValid;
}

bool QgsCoordinateReferenceSystem::createFromProj4( const QString &theProj4String )
{
  d.detach();

  //
  // Examples:
  // +proj=tmerc +lat_0=0 +lon_0=-62 +k=0.999500 +x_0=400000 +y_0=0
  // +ellps=clrk80 +towgs84=-255,-15,71,0,0,0,0 +units=m +no_defs
  //
  // +proj=lcc +lat_1=46.8 +lat_0=46.8 +lon_0=2.337229166666664 +k_0=0.99987742
  // +x_0=600000 +y_0=2200000 +a=6378249.2 +b=6356515.000000472 +units=m +no_defs
  //
  QString myProj4String = theProj4String.trimmed();
  //QgsDebugMsg( "proj4: " + myProj4String );
  d->mIsValid = false;
  d->mWkt.clear();

  QRegExp myProjRegExp( "\\+proj=(\\S+)" );
  int myStart = myProjRegExp.indexIn( myProj4String );
  if ( myStart == -1 )
  {
    QgsDebugMsg( "proj string supplied has no +proj argument" );
    return d->mIsValid;
  }

  d->mProjectionAcronym = myProjRegExp.cap( 1 );

  QRegExp myEllipseRegExp( "\\+ellps=(\\S+)" );
  myStart = myEllipseRegExp.indexIn( myProj4String );
  if ( myStart == -1 )
  {
    QgsDebugMsg( "proj string supplied has no +ellps argument" );
    d->mEllipsoidAcronym.clear();
  }
  else
  {
    d->mEllipsoidAcronym = myEllipseRegExp.cap( 1 );
  }

  QRegExp myAxisRegExp( "\\+a=(\\S+)" );
  myStart = myAxisRegExp.indexIn( myProj4String );
  if ( myStart == -1 )
  {
    QgsDebugMsg( "proj string supplied has no +a argument" );
  }

  /*
   * We try to match the proj string to and srsid using the following logic:
   *
   * - perform a whole text search on srs name (if not null). The srs name will
   *   have been set if this method has been delegated to from createFromWkt.
   * Normally we wouldnt expect this to work, but its worth trying first
   * as its quicker than methods below..
   */
  long mySrsId = 0;
  QgsCoordinateReferenceSystem::RecordMap myRecord;

  /*
   * - if the above does not match perform a whole text search on proj4 string (if not null)
   */
  // QgsDebugMsg( "wholetext match on name failed, trying proj4string match" );
  myRecord = getRecord( "select * from tbl_srs where parameters=" + quotedValue( myProj4String ) + " order by deprecated" );
  if ( myRecord.empty() )
  {
    // Ticket #722 - aaronr
    // Check if we can swap the lat_1 and lat_2 params (if they exist) to see if we match...
    // First we check for lat_1 and lat_2
    QRegExp myLat1RegExp( "\\+lat_1=\\S+" );
    QRegExp myLat2RegExp( "\\+lat_2=\\S+" );
    int myStart1 = 0;
    int myLength1 = 0;
    int myStart2 = 0;
    int myLength2 = 0;
    QString lat1Str = "";
    QString lat2Str = "";
    myStart1 = myLat1RegExp.indexIn( myProj4String, myStart1 );
    myStart2 = myLat2RegExp.indexIn( myProj4String, myStart2 );
    if ( myStart1 != -1 && myStart2 != -1 )
    {
      myLength1 = myLat1RegExp.matchedLength();
      myLength2 = myLat2RegExp.matchedLength();
      lat1Str = myProj4String.mid( myStart1 + LAT_PREFIX_LEN, myLength1 - LAT_PREFIX_LEN );
      lat2Str = myProj4String.mid( myStart2 + LAT_PREFIX_LEN, myLength2 - LAT_PREFIX_LEN );
    }
    // If we found the lat_1 and lat_2 we need to swap and check to see if we can find it...
    if ( lat1Str != "" && lat2Str != "" )
    {
      // Make our new string to check...
      QString theProj4StringModified = myProj4String;
      // First just swap in the lat_2 value for lat_1 value
      theProj4StringModified.replace( myStart1 + LAT_PREFIX_LEN, myLength1 - LAT_PREFIX_LEN, lat2Str );
      // Now we have to find the lat_2 location again since it has potentially moved...
      myStart2 = 0;
      myStart2 = myLat2RegExp.indexIn( theProj4String, myStart2 );
      theProj4StringModified.replace( myStart2 + LAT_PREFIX_LEN, myLength2 - LAT_PREFIX_LEN, lat1Str );
      QgsDebugMsg( "trying proj4string match with swapped lat_1,lat_2" );
      myRecord = getRecord( "select * from tbl_srs where parameters=" + quotedValue( theProj4StringModified.trimmed() ) + " order by deprecated" );
    }
  }

  if ( myRecord.empty() )
  {
    // match all parameters individually:
    // - order of parameters doesn't matter
    // - found definition may have more parameters (like +towgs84 in GDAL)
    // - retry without datum, if no match is found (looks like +datum<>WGS84 was dropped in GDAL)

    QString sql = "SELECT * FROM tbl_srs WHERE ";
    QString delim = "";
    QString datum;

    // split on spaces followed by a plus sign (+) to deal
    // also with parameters containing spaces (e.g. +nadgrids)
    // make sure result is trimmed (#5598)
    QStringList myParams;
    Q_FOREACH ( const QString& param, myProj4String.split( QRegExp( "\\s+(?=\\+)" ), QString::SkipEmptyParts ) )
    {
      QString arg = QString( "' '||parameters||' ' LIKE %1" ).arg( quotedValue( QString( "% %1 %" ).arg( param.trimmed() ) ) );
      if ( param.startsWith( "+datum=" ) )
      {
        datum = arg;
      }
      else
      {
        sql += delim + arg;
        delim = " AND ";
        myParams << param.trimmed();
      }
    }

    if ( !datum.isEmpty() )
    {
      myRecord = getRecord( sql + delim + datum + " order by deprecated" );
    }

    if ( myRecord.empty() )
    {
      // datum might have disappeared in definition - retry without it
      myRecord = getRecord( sql + " order by deprecated" );
    }

    if ( !myRecord.empty() )
    {
      // Bugfix 8487 : test param lists are equal, except for +datum
      QStringList foundParams;
      Q_FOREACH ( const QString& param, myRecord["parameters"].split( QRegExp( "\\s+(?=\\+)" ), QString::SkipEmptyParts ) )
      {
        if ( !param.startsWith( "+datum=" ) )
          foundParams << param.trimmed();
      }

      myParams.sort();
      foundParams.sort();

      if ( myParams != foundParams )
      {
        myRecord.clear();
      }
    }
  }

  if ( !myRecord.empty() )
  {
    mySrsId = myRecord["srs_id"].toLong();
    //QgsDebugMsg( "proj4string param match search for srsid returned srsid: " + QString::number( mySrsId ) );
    if ( mySrsId > 0 )
    {
      createFromSrsId( mySrsId );
    }
  }
  else
  {
    // Last ditch attempt to piece together what we know of the projection to find a match...
    QgsDebugMsg( "globbing search for srsid from this proj string" );
    setProj4String( myProj4String );
    mySrsId = findMatchingProj();
   // QgsDebugMsg( "globbing search for srsid returned srsid: " + QString::number( mySrsId ) );
    if ( mySrsId > 0 )
    {
      createFromSrsId( mySrsId );
    }
    else
    {
      d->mIsValid = false;
    }
  }

  // if we failed to look up the projection in database, don't worry. we can still use it :)
  if ( !d->mIsValid )
  {
    QgsDebugMsg( "Projection is not found in databases." );
    //setProj4String will set mIsValidFlag to true if there is no issue
    setProj4String( myProj4String );
  }

  return d->mIsValid;
}

//private method meant for internal use by this class only
QgsCoordinateReferenceSystem::RecordMap QgsCoordinateReferenceSystem::getRecord( const QString& theSql )
{
  QString myDatabaseFileName;
  QgsCoordinateReferenceSystem::RecordMap myMap;
  QString myFieldName;
  QString myFieldValue;
  sqlite3      *myDatabase;
  const char   *myTail;
  sqlite3_stmt *myPreparedStatement;
  int           myResult;

//  QgsDebugMsg( "running query: " + theSql );
  // Get the full path name to the sqlite3 spatial reference database.
  myDatabaseFileName = srsDbFilePath;
  QFileInfo myInfo( myDatabaseFileName );
  if ( !myInfo.exists() )
  {
//    QgsDebugMsg( "failed : " + myDatabaseFileName + " does not exist!" );
    return myMap;
  }

  //check the db is available
  myResult = openDb( myDatabaseFileName, &myDatabase );
  if ( myResult != SQLITE_OK )
  {
    return myMap;
  }

  myResult = sqlite3_prepare( myDatabase, theSql.toUtf8(), theSql.toUtf8().length(), &myPreparedStatement, &myTail );
  // XXX Need to free memory from the error msg if one is set
  if ( myResult == SQLITE_OK && sqlite3_step( myPreparedStatement ) == SQLITE_ROW )
  {
    QgsDebugMsg( "trying system srs.db" );
    int myColumnCount = sqlite3_column_count( myPreparedStatement );
    //loop through each column in the record adding its expression name and value to the map
    for ( int myColNo = 0; myColNo < myColumnCount; myColNo++ )
    {
      myFieldName = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_name( myPreparedStatement, myColNo ) ) );
      myFieldValue = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, myColNo ) ) );
      myMap[myFieldName] = myFieldValue;
    }
    if ( sqlite3_step( myPreparedStatement ) != SQLITE_DONE )
    {
      QgsDebugMsg( "Multiple records found in srs.db" );
      myMap.clear();
    }
  }
  else
  {
    //QgsDebugMsg( "failed :  " + theSql );
  }

  if ( myMap.empty() )
  {
    QgsDebugMsg( "trying user qgis.db" );
    sqlite3_finalize( myPreparedStatement );
    sqlite3_close( myDatabase );

    myDatabaseFileName = qgisUserDbFilePath;
    QFileInfo myFileInfo;
    myFileInfo.setFile( myDatabaseFileName );
    if ( !myFileInfo.exists() )
    {
      QgsDebugMsg( "user qgis.db not found" );
      return myMap;
    }

    //check the db is available
    myResult = openDb( myDatabaseFileName, &myDatabase );
    if ( myResult != SQLITE_OK )
    {
      return myMap;
    }

    myResult = sqlite3_prepare( myDatabase, theSql.toUtf8(), theSql.toUtf8().length(), &myPreparedStatement, &myTail );
    // XXX Need to free memory from the error msg if one is set
    if ( myResult == SQLITE_OK && sqlite3_step( myPreparedStatement ) == SQLITE_ROW )
    {
      int myColumnCount = sqlite3_column_count( myPreparedStatement );
      //loop through each column in the record adding its field name and value to the map
      for ( int myColNo = 0; myColNo < myColumnCount; myColNo++ )
      {
        myFieldName = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_name( myPreparedStatement, myColNo ) ) );
        myFieldValue = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, myColNo ) ) );
        myMap[myFieldName] = myFieldValue;
      }

      if ( sqlite3_step( myPreparedStatement ) != SQLITE_DONE )
      {
        QgsDebugMsg( "Multiple records found in srs.db" );
        myMap.clear();
      }
    }
    else
    {
      //QgsDebugMsg( "failed :  " + theSql );
    }
  }
  sqlite3_finalize( myPreparedStatement );
  sqlite3_close( myDatabase );

#ifdef QGISDEBUG
  QgsDebugMsg( "retrieved:  " + theSql );
  RecordMap::Iterator it;
  for ( it = myMap.begin(); it != myMap.end(); ++it )
  {
    QgsDebugMsgLevel( it.key() + " => " + it.value(), 2 );
  }
#endif

  return myMap;
}

// Accessors -----------------------------------

long QgsCoordinateReferenceSystem::srsid() const
{
  return d->mSrsId;
}

long QgsCoordinateReferenceSystem::postgisSrid() const
{
  return d->mSRID;
}

QString QgsCoordinateReferenceSystem::authid() const
{
  return d->mAuthId;
}

QString QgsCoordinateReferenceSystem::description() const
{
  if ( d->mDescription.isNull() )
  {
    return "";
  }
  else
  {
    return d->mDescription;
  }
}

QString QgsCoordinateReferenceSystem::projectionAcronym() const
{
  if ( d->mProjectionAcronym.isNull() )
  {
    return "";
  }
  else
  {
    return d->mProjectionAcronym;
  }
}

QString QgsCoordinateReferenceSystem::ellipsoidAcronym() const
{
  if ( d->mEllipsoidAcronym.isNull() )
  {
    return "";
  }
  else
  {
    return d->mEllipsoidAcronym;
  }
}

QString QgsCoordinateReferenceSystem::toProj4() const
{
  if ( !d->mIsValid )
    return "";

  if ( d->mProj4.isEmpty() )
  {
    char *proj4src = nullptr;
    OSRExportToProj4( d->mCRS, &proj4src );
    d->mProj4 = proj4src;
    CPLFree( proj4src );
  }
  // Stray spaces at the end?
  return d->mProj4.trimmed();
}

bool QgsCoordinateReferenceSystem::geographicFlag() const
{
  return d->mIsGeographic;
}

UnitType QgsCoordinateReferenceSystem::mapUnits() const
{
  return d->mMapUnits;
}


// Mutators -----------------------------------


void QgsCoordinateReferenceSystem::setInternalId( long theSrsId )
{
  d.detach();
  d->mSrsId = theSrsId;
}
void QgsCoordinateReferenceSystem::setAuthId( const QString& authId )
{
  d.detach();
  d->mAuthId = authId;
}
void QgsCoordinateReferenceSystem::setSrid( long theSrid )
{
  d.detach();
  d->mSRID = theSrid;
}
void QgsCoordinateReferenceSystem::setDescription( const QString& theDescription )
{
  d.detach();
  d->mDescription = theDescription;
}
void QgsCoordinateReferenceSystem::setProj4String( const QString& theProj4String )
{
  d.detach();
  d->mProj4 = theProj4String;

  QgsLocaleNumC l;

  OSRDestroySpatialReference( d->mCRS );
  d->mCRS = OSRNewSpatialReference( nullptr );
  d->mIsValid = OSRImportFromProj4( d->mCRS, theProj4String.trimmed().toLatin1().constData() ) == OGRERR_NONE;
  // OSRImportFromProj4() may accept strings that are not valid proj.4 strings,
  // e.g if they lack a +ellps parameter, it will automatically add +ellps=WGS84, but as
  // we use the original mProj4 with QgsCoordinateTransform, it will fail to initialize
  // so better detect it now.
  projPJ theProj = pj_init_plus( theProj4String.trimmed().toLatin1().constData() );
  if ( !theProj )
  {
    QgsDebugMsg( "proj.4 string rejected by pj_init_plus()" );
    d->mIsValid = false;
  }
  else
  {
    pj_free( theProj );
  }
  d->mWkt.clear();
  setMapUnits();

#if defined(QGISDEBUG) && QGISDEBUG>=3
  debugPrint();
#endif
}
void QgsCoordinateReferenceSystem::setGeographicFlag( bool theGeoFlag )
{
  d.detach();
  d->mIsGeographic = theGeoFlag;
}
void QgsCoordinateReferenceSystem::setEpsg( long theEpsg )
{
  d.detach();
  d->mAuthId = QString( "EPSG:%1" ).arg( theEpsg );
}
void  QgsCoordinateReferenceSystem::setProjectionAcronym( const QString& theProjectionAcronym )
{
  d.detach();
  d->mProjectionAcronym = theProjectionAcronym;
}
void  QgsCoordinateReferenceSystem::setEllipsoidAcronym( const QString& theEllipsoidAcronym )
{
  d.detach();
  d->mEllipsoidAcronym = theEllipsoidAcronym;
}

void QgsCoordinateReferenceSystem::setMapUnits()
{
  d.detach();
  if ( !d->mIsValid )
  {
    d->mMapUnits = UnknownUnit;
    return;
  }

  char *unitName;

  // Of interest to us is that this call adds in a unit parameter if
  // one doesn't already exist.
  OSRFixup( d->mCRS );

  if ( OSRIsProjected( d->mCRS ) )
  {
    double toMeter = OSRGetLinearUnits( d->mCRS, &unitName );
    QString unit( unitName );

    // If the units parameter was created during the Fixup() call
    // above, the name of the units is likely to be 'unknown'. Try to
    // do better than that ... (but perhaps ogr should be enhanced to
    // do this instead?).

    static const double feetToMeter = 0.3048;
    static const double smallNum = 1e-3;

    if ( qAbs( toMeter - feetToMeter ) < smallNum )
      unit = "Foot";

    //QgsDebugMsg( "Projection has linear units of " + unit );

    if ( qgsDoubleNear( toMeter, 1.0 ) ) //Unit name for meters would be "metre"
      d->mMapUnits = Meters;
    else if ( unit == "Foot" )
      d->mMapUnits = Feet;
    else
    {
     // QgsDebugMsg( "Unsupported map units of " + unit );
      d->mMapUnits = UnknownUnit;
    }
  }
  else
  {
    OSRGetAngularUnits( d->mCRS, &unitName );
    QString unit( unitName );
    if ( unit == "degree" )
      d->mMapUnits = Degrees;
    else
    {
    //  QgsDebugMsg( "Unsupported map units of " + unit );
      d->mMapUnits = UnknownUnit;
    }
//    QgsDebugMsgLevel( "Projection has angular units of " + unit, 3 );
  }
}

/*
*    check if srs is a geocs or a proj cs (using ogr isGeographic)
*   then sequentially walk through the database (first users qgis.db srs tbl then
*   system srs.db tbl), converting each entry into an ogr srs and using isSame
*   or isSameGeocs (essentially calling the == overloaded operator). We'll try to
*   be smart about this and first parse out the proj and ellpse strings and only
*   check for a match in entities that have the same ellps and proj entries so
*   that it doesnt munch yer cpu so much.
*/
long QgsCoordinateReferenceSystem::findMatchingProj()
{
  if ( d->mEllipsoidAcronym.isNull() || d->mProjectionAcronym.isNull()
       || !d->mIsValid )
  {
    QgsDebugMsg( "QgsCoordinateReferenceSystem::findMatchingProj will only "
                 "work if prj acr ellipsoid acr and proj4string are set"
                 " and the current projection is valid!" );
    return 0;
  }

  sqlite3      *myDatabase;
  const char   *myTail;
  sqlite3_stmt *myPreparedStatement;
  int           myResult;

  // Set up the query to retrieve the projection information
  // needed to populate the list
  QString mySql = QString( "select srs_id,parameters from tbl_srs where "
                           "projection_acronym=%1 and ellipsoid_acronym=%2 order by deprecated" )
                  .arg( quotedValue( d->mProjectionAcronym ),
                        quotedValue( d->mEllipsoidAcronym ) );
  // Get the full path name to the sqlite3 spatial reference database.
  QString myDatabaseFileName = srsDbFilePath;

  //check the db is available
  myResult = openDb( myDatabaseFileName, &myDatabase );
  if ( myResult != SQLITE_OK )
  {
    return 0;
  }

  myResult = sqlite3_prepare( myDatabase, mySql.toUtf8(), mySql.toUtf8().length(), &myPreparedStatement, &myTail );
// XXX Need to free memory from the error msg if one is set
  if ( myResult == SQLITE_OK )
  {

    while ( sqlite3_step( myPreparedStatement ) == SQLITE_ROW )
    {
      QString mySrsId = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 0 ) ) );
      QString myProj4String = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 1 ) ) );
      if ( toProj4() == myProj4String.trimmed() )
      {
      //  QgsDebugMsg( "-------> MATCH FOUND in srs.db srsid: " + mySrsId );
        // close the sqlite3 statement
        sqlite3_finalize( myPreparedStatement );
        sqlite3_close( myDatabase );
        return mySrsId.toLong();
      }
      else
      {
// QgsDebugMsg(QString(" Not matched : %1").arg(myProj4String));
      }
    }
  }
  QgsDebugMsg( "no match found in srs.db, trying user db now!" );
  // close the sqlite3 statement
  sqlite3_finalize( myPreparedStatement );
  sqlite3_close( myDatabase );
  //
  // Try the users db now
  //

  myDatabaseFileName = qgisUserDbFilePath;
  //check the db is available
  myResult = openDb( myDatabaseFileName, &myDatabase );
  if ( myResult != SQLITE_OK )
  {
    return 0;
  }

  myResult = sqlite3_prepare( myDatabase, mySql.toUtf8(), mySql.toUtf8().length(), &myPreparedStatement, &myTail );
// XXX Need to free memory from the error msg if one is set
  if ( myResult == SQLITE_OK )
  {

    while ( sqlite3_step( myPreparedStatement ) == SQLITE_ROW )
    {
      QString mySrsId = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 0 ) ) );
      QString myProj4String = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 1 ) ) );
      if ( toProj4() == myProj4String.trimmed() )
      {
      //  QgsDebugMsg( "-------> MATCH FOUND in user qgis.db srsid: " + mySrsId );
        // close the sqlite3 statement
        sqlite3_finalize( myPreparedStatement );
        sqlite3_close( myDatabase );
        return mySrsId.toLong();
      }
      else
      {
// QgsDebugMsg(QString(" Not matched : %1").arg(myProj4String));
      }
    }
  }
  QgsDebugMsg( "no match found in user db" );

  // close the sqlite3 statement
  sqlite3_finalize( myPreparedStatement );
  sqlite3_close( myDatabase );
  return 0;
}

bool QgsCoordinateReferenceSystem::operator==( const QgsCoordinateReferenceSystem &theSrs ) const
{
  return ( !d->mIsValid && !theSrs.d->mIsValid ) ||
         ( d->mIsValid && theSrs.d->mIsValid && theSrs.authid() == authid() );
}

bool QgsCoordinateReferenceSystem::operator!=( const QgsCoordinateReferenceSystem &theSrs ) const
{
  return  !( *this == theSrs );
}

QString QgsCoordinateReferenceSystem::toWkt() const
{
  if ( d->mWkt.isEmpty() )
  {
    char *wkt;
    if ( OSRExportToWkt( d->mCRS, &wkt ) == OGRERR_NONE )
    {
      d->mWkt = wkt;
      OGRFree( wkt );
    }
  }
  return d->mWkt;
}

bool QgsCoordinateReferenceSystem::readXML( const QDomNode & theNode )
{
  d.detach();
  QgsDebugMsg( "Reading Spatial Ref Sys from xml ------------------------!" );
  bool result = true;
  QDomNode srsNode  = theNode.namedItem( "spatialrefsys" );

  if ( ! srsNode.isNull() )
  {
    bool initialized = false;

    long srsid = srsNode.namedItem( "srsid" ).toElement().text().toLong();

    QDomNode myNode;

    if ( srsid < USER_CRS_START_ID )
    {
      myNode = srsNode.namedItem( "authid" );
      if ( !myNode.isNull() )
      {
        //operator=( QgsCRSCache::instance()->crsByOgcWmsCrs( myNode.toElement().text() ) );
        if ( isValid() )
        {
          initialized = true;
        }
      }

      if ( !initialized )
      {
        myNode = srsNode.namedItem( "epsg" );
        if ( !myNode.isNull() )
        {
          //operator=( QgsCRSCache::instance()->crsByEpsgId( myNode.toElement().text().toLong() ) );
          if ( isValid() )
          {
            initialized = true;
          }
        }
      }
    }
    else
    {
      QgsDebugMsg( "Ignoring authid/epsg for user crs." );
    }

    if ( initialized )
    {
      QgsDebugMsg( "Set from auth id" );
    }
    else
    {
      myNode = srsNode.namedItem( "proj4" );

      if ( createFromProj4( myNode.toElement().text() ) )
      {
        // createFromProj4() sets everything, including map units
        QgsDebugMsg( "Setting from proj4 string" );
      }
      else
      {
        QgsDebugMsg( "Setting from elements one by one" );

        myNode = srsNode.namedItem( "proj4" );
        setProj4String( myNode.toElement().text() );

        myNode = srsNode.namedItem( "srsid" );
        setInternalId( myNode.toElement().text().toLong() );

        myNode = srsNode.namedItem( "srid" );
        setSrid( myNode.toElement().text().toLong() );

        myNode = srsNode.namedItem( "authid" );
        setAuthId( myNode.toElement().text() );

        myNode = srsNode.namedItem( "description" );
        setDescription( myNode.toElement().text() );

        myNode = srsNode.namedItem( "projectionacronym" );
        setProjectionAcronym( myNode.toElement().text() );

        myNode = srsNode.namedItem( "ellipsoidacronym" );
        setEllipsoidAcronym( myNode.toElement().text() );

        myNode = srsNode.namedItem( "geographicflag" );
        if ( myNode.toElement().text().compare( "true" ) )
        {
          setGeographicFlag( true );
        }
        else
        {
          setGeographicFlag( false );
        }

        //make sure the map units have been set
        setMapUnits();

        //@TODO this srs needs to be validated!!!
        d->mIsValid = true; //shamelessly hard coded for now
      }
      //TODO: createFromProj4 used to save to the user database any new CRS
      // this behavior was changed in order to separate creation and saving.
      // Not sure if it necessary to save it here, should be checked by someone
      // familiar with the code (should also give a more descriptive name to the generated CRS)
      if ( d->mSrsId == 0 )
      {
        QString myName = QString( " * %1 (%2)" )
                         .arg( QObject::tr( "Generated CRS", "A CRS automatically generated from layer info get this prefix for description" ),
                               toProj4() );
        saveAsUserCRS( myName );
      }

    }
  }
  else
  {
    // Return default CRS if none was found in the XML.
    createFromId( GEOCRS_ID, InternalCrsId );
    result = false;
  }
  return result;
}

bool QgsCoordinateReferenceSystem::writeXML( QDomNode & theNode, QDomDocument & theDoc ) const
{

  QDomElement myLayerNode = theNode.toElement();
  QDomElement mySrsElement  = theDoc.createElement( "spatialrefsys" );

  QDomElement myProj4Element  = theDoc.createElement( "proj4" );
  myProj4Element.appendChild( theDoc.createTextNode( toProj4() ) );
  mySrsElement.appendChild( myProj4Element );

  QDomElement mySrsIdElement  = theDoc.createElement( "srsid" );
  mySrsIdElement.appendChild( theDoc.createTextNode( QString::number( srsid() ) ) );
  mySrsElement.appendChild( mySrsIdElement );

  QDomElement mySridElement  = theDoc.createElement( "srid" );
  mySridElement.appendChild( theDoc.createTextNode( QString::number( postgisSrid() ) ) );
  mySrsElement.appendChild( mySridElement );

  QDomElement myEpsgElement  = theDoc.createElement( "authid" );
  myEpsgElement.appendChild( theDoc.createTextNode( authid() ) );
  mySrsElement.appendChild( myEpsgElement );

  QDomElement myDescriptionElement  = theDoc.createElement( "description" );
  myDescriptionElement.appendChild( theDoc.createTextNode( description() ) );
  mySrsElement.appendChild( myDescriptionElement );

  QDomElement myProjectionAcronymElement  = theDoc.createElement( "projectionacronym" );
  myProjectionAcronymElement.appendChild( theDoc.createTextNode( projectionAcronym() ) );
  mySrsElement.appendChild( myProjectionAcronymElement );

  QDomElement myEllipsoidAcronymElement  = theDoc.createElement( "ellipsoidacronym" );
  myEllipsoidAcronymElement.appendChild( theDoc.createTextNode( ellipsoidAcronym() ) );
  mySrsElement.appendChild( myEllipsoidAcronymElement );

  QDomElement myGeographicFlagElement  = theDoc.createElement( "geographicflag" );
  QString myGeoFlagText = "false";
  if ( geographicFlag() )
  {
    myGeoFlagText = "true";
  }

  myGeographicFlagElement.appendChild( theDoc.createTextNode( myGeoFlagText ) );
  mySrsElement.appendChild( myGeographicFlagElement );

  myLayerNode.appendChild( mySrsElement );

  return true;
}



//
// Static helper methods below this point only please!
//


// Returns the whole proj4 string for the selected srsid
//this is a static method! NOTE I've made it private for now to reduce API clutter TS
QString QgsCoordinateReferenceSystem::proj4FromSrsId( const int theSrsId )
{

  QString myDatabaseFileName;
  QString myProjString;
  QString mySql = QString( "select parameters from tbl_srs where srs_id = %1 order by deprecated" ).arg( theSrsId );

 // QgsDebugMsg( "mySrsId = " + QString::number( theSrsId ) );
  //QgsDebugMsg( "USER_CRS_START_ID = " + QString::number( USER_CRS_START_ID ) );
 // QgsDebugMsg( "Selection sql : " + mySql );

  //
  // Determine if this is a user projection or a system on
  // user projection defs all have srs_id >= 100000
  //
  if ( theSrsId >= USER_CRS_START_ID )
  {
    myDatabaseFileName = qgisUserDbFilePath;
    QFileInfo myFileInfo;
    myFileInfo.setFile( myDatabaseFileName );
    if ( !myFileInfo.exists() ) //its unlikely that this condition will ever be reached
    {
      QgsDebugMsg( "users qgis.db not found" );
      return QString();
    }
  }
  else //must be  a system projection then
  {
    myDatabaseFileName = srsDbFilePath;
  }
  //QgsDebugMsg( "db = " + myDatabaseFileName );

  sqlite3 *db;
  int rc;
  rc = openDb( myDatabaseFileName, &db );
  if ( rc )
  {
    return QString();
  }
  // prepare the sql statement
  const char *pzTail;
  sqlite3_stmt *ppStmt;

  rc = sqlite3_prepare( db, mySql.toUtf8(), mySql.toUtf8().length(), &ppStmt, &pzTail );
  // XXX Need to free memory from the error msg if one is set

  if ( rc == SQLITE_OK )
  {
    if ( sqlite3_step( ppStmt ) == SQLITE_ROW )
    {
      myProjString = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( ppStmt, 0 ) ) );
    }
  }
  // close the statement
  sqlite3_finalize( ppStmt );
  // close the database
  sqlite3_close( db );

  //Q_ASSERT(myProjString.length() > 0);
  return myProjString;
}

int QgsCoordinateReferenceSystem::openDb( const QString& path, sqlite3 **db, bool readonly )
{
 // QgsDebugMsgLevel( "path = " + path, 3 );
  int myResult = readonly
                 ? sqlite3_open_v2( path.toUtf8().data(), db, SQLITE_OPEN_READONLY, nullptr )
                 : sqlite3_open( path.toUtf8().data(), db );

  if ( myResult != SQLITE_OK )
  {
//    QgsDebugMsg( "Can't open database: " + QString( sqlite3_errmsg( *db ) ) );
    // XXX This will likely never happen since on open, sqlite creates the
    //     database if it does not exist.
    // ... unfortunately it happens on Windows
    //QgsMessageLog::logMessage( QObject::tr( "Could not open CRS database %1\nError(%2): %3" )
    //                           .arg( path )
    //                           .arg( myResult )
    //                           .arg( sqlite3_errmsg( *db ) ), QObject::tr( "CRS" ) );
  }
  return myResult;
}

void QgsCoordinateReferenceSystem::setCustomSrsValidation( CUSTOM_CRS_VALIDATION f )
{
  mCustomSrsValidation = f;
}

CUSTOM_CRS_VALIDATION QgsCoordinateReferenceSystem::customSrsValidation()
{
  return mCustomSrsValidation;
}

void QgsCoordinateReferenceSystem::debugPrint()
{
  //QgsDebugMsg( "***SpatialRefSystem***" );
  //QgsDebugMsg( "* Valid : " + ( d->mIsValid ? QString( "true" ) : QString( "false" ) ) );
  //QgsDebugMsg( "* SrsId : " + QString::number( d->mSrsId ) );
  //QgsDebugMsg( "* Proj4 : " + toProj4() );
  //QgsDebugMsg( "* WKT   : " + toWkt() );
  //QgsDebugMsg( "* Desc. : " + d->mDescription );
  if ( mapUnits() == Meters )
  {
    QgsDebugMsg( "* Units : meters" );
  }
  else if ( mapUnits() == Feet )
  {
    QgsDebugMsg( "* Units : feet" );
  }
  else if ( mapUnits() == Degrees )
  {
    QgsDebugMsg( "* Units : degrees" );
  }
}

void QgsCoordinateReferenceSystem::setValidationHint( const QString& html )
{
  d.detach();
  d->mValidationHint = html;
}

QString QgsCoordinateReferenceSystem::validationHint()
{
  return d->mValidationHint;
}

/// Copied from QgsCustomProjectionDialog ///
/// Please refactor into SQL handler !!!  ///

bool QgsCoordinateReferenceSystem::saveAsUserCRS( const QString& name )
{
  if ( !d->mIsValid )
  {
    QgsDebugMsg( "Can't save an invalid CRS!" );
    return false;
  }

  QString mySql;

  QString proj4String = d->mProj4;
  if ( proj4String.isEmpty() )
  {
    proj4String = toProj4();
  }

  //if this is the first record we need to ensure that its srs_id is 10000. For
  //any rec after that sqlite3 will take care of the autonumering
  //this was done to support sqlite 3.0 as it does not yet support
  //the autoinc related system tables.
  if ( getRecordCount() == 0 )
  {
    mySql = "insert into tbl_srs (srs_id,description,projection_acronym,ellipsoid_acronym,parameters,is_geo) values ("
            + QString::number( USER_CRS_START_ID )
            + ',' + quotedValue( name )
            + ',' + quotedValue( projectionAcronym() )
            + ',' + quotedValue( ellipsoidAcronym() )
            + ',' + quotedValue( toProj4() )
            + ",0)"; // <-- is_geo shamelessly hard coded for now
  }
  else
  {
    mySql = "insert into tbl_srs (description,projection_acronym,ellipsoid_acronym,parameters,is_geo) values ("
            + quotedValue( name )
            + ',' + quotedValue( projectionAcronym() )
            + ',' + quotedValue( ellipsoidAcronym() )
            + ',' + quotedValue( toProj4() )
            + ",0)"; // <-- is_geo shamelessly hard coded for now
  }
  sqlite3      *myDatabase;
  const char   *myTail;
  sqlite3_stmt *myPreparedStatement;
  int           myResult;
  //check the db is available
  myResult = sqlite3_open( qgisUserDbFilePath.toUtf8().data(), &myDatabase );
  if ( myResult != SQLITE_OK )
  {
  /*  QgsDebugMsg( QString( "Can't open or create database %1: %2" )
                 .arg( QgsApplication::qgisUserDbFilePath(),
                       sqlite3_errmsg( myDatabase ) ) );*/
    return false;
  }
  //QgsDebugMsg( QString( "Update or insert sql \n%1" ).arg( mySql ) );
  myResult = sqlite3_prepare( myDatabase, mySql.toUtf8(), mySql.toUtf8().length(), &myPreparedStatement, &myTail );

  qint64 return_id;
  if ( myResult == SQLITE_OK && sqlite3_step( myPreparedStatement ) == SQLITE_DONE )
  {
    //QgsMessageLog::logMessage( QObject::tr( "Saved user CRS [%1]" ).arg( toProj4() ), QObject::tr( "CRS" ) );

    return_id = sqlite3_last_insert_rowid( myDatabase );
    setInternalId( return_id );

    //We add the just created user CRS to the list of recently used CRS
    QSettings settings;
    //QStringList recentProjections = settings.value( "/UI/recentProjections" ).toStringList();
    QStringList projectionsProj4 = settings.value( "/UI/recentProjectionsProj4" ).toStringList();
    QStringList projectionsAuthId = settings.value( "/UI/recentProjectionsAuthId" ).toStringList();
    //recentProjections.append();
    //settings.setValue( "/UI/recentProjections", recentProjections );
    projectionsProj4.append( toProj4() );
    projectionsAuthId.append( authid() );
    settings.setValue( "/UI/recentProjectionsProj4", projectionsProj4 );
    settings.setValue( "/UI/recentProjectionsAuthId", projectionsAuthId );

  }
  else
    return_id = -1;
  return return_id;
}

long QgsCoordinateReferenceSystem::getRecordCount()
{
  sqlite3      *myDatabase;
  const char   *myTail;
  sqlite3_stmt *myPreparedStatement;
  int           myResult;
  long          myRecordCount = 0;
  //check the db is available
  myResult = sqlite3_open_v2( qgisUserDbFilePath.toUtf8().data(), &myDatabase, SQLITE_OPEN_READONLY, nullptr );
  if ( myResult != SQLITE_OK )
  {
//    QgsDebugMsg( QString( "Can't open database: %1" ).arg( sqlite3_errmsg( myDatabase ) ) );
    return 0;
  }
  // Set up the query to retrieve the projection information needed to populate the ELLIPSOID list
  QString mySql = "select count(*) from tbl_srs";
  myResult = sqlite3_prepare( myDatabase, mySql.toUtf8(), mySql.toUtf8().length(), &myPreparedStatement, &myTail );
  // XXX Need to free memory from the error msg if one is set
  if ( myResult == SQLITE_OK )
  {
    if ( sqlite3_step( myPreparedStatement ) == SQLITE_ROW )
    {
      QString myRecordCountString = QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( myPreparedStatement, 0 ) ) );
      myRecordCount = myRecordCountString.toLong();
    }
  }
  // close the sqlite3 statement
  sqlite3_finalize( myPreparedStatement );
  sqlite3_close( myDatabase );
  return myRecordCount;
}

QString QgsCoordinateReferenceSystem::quotedValue( QString value )
{
  value.replace( '\'', "''" );
  return value.prepend( '\'' ).append( '\'' );
}

// adapted from gdal/ogr/ogr_srs_dict.cpp
bool QgsCoordinateReferenceSystem::loadWkts( QHash<int, QString> &wkts, const char *filename )
{
  qDebug( "Loading %s", filename );
  const char *pszFilename = CPLFindFile( "gdal", filename );
  if ( !pszFilename )
    return false;

  QFile csv( pszFilename );
  if ( !csv.open( QIODevice::ReadOnly ) )
    return false;

  QTextStream lines( &csv );

  for ( ;; )
  {
    QString line = lines.readLine();
    if ( line.isNull() )
      break;

    if ( line.startsWith( '#' ) )
    {
      continue;
    }
    else if ( line.startsWith( "include " ) )
    {
      if ( !loadWkts( wkts, line.mid( 8 ).toUtf8() ) )
        break;
    }
    else
    {
      int pos = line.indexOf( ',' );
      if ( pos < 0 )
        return false;

      bool ok;
      int epsg = line.left( pos ).toInt( &ok );
      if ( !ok )
        return false;

      wkts.insert( epsg, line.mid( pos + 1 ) );
    }
  }

  csv.close();

  return true;
}

bool QgsCoordinateReferenceSystem::loadIDs( QHash<int, QString> &wkts )
{
  OGRSpatialReferenceH crs = OSRNewSpatialReference( nullptr );

  Q_FOREACH ( const QString& csv, QStringList() << "gcs.csv" << "pcs.csv" << "vertcs.csv" << "compdcs.csv" << "geoccs.csv" )
  {
    QString filename = CPLFindFile( "gdal", csv.toUtf8() );

    QFile f( filename );
    if ( !f.open( QIODevice::ReadOnly ) )
      continue;

    QTextStream lines( &f );
    int l = 0, n = 0;

    lines.readLine();
    for ( ;; )
    {
      l++;
      QString line = lines.readLine();
      if ( line.isNull() )
        break;

      int pos = line.indexOf( ',' );
      if ( pos < 0 )
        continue;

      bool ok;
      int epsg = line.left( pos ).toInt( &ok );
      if ( !ok )
        continue;

      // some CRS are known to fail (see http://trac.osgeo.org/gdal/ticket/2900)
      if ( epsg == 2218 || epsg == 2221 || epsg == 2296 || epsg == 2297 || epsg == 2298 || epsg == 2299 || epsg == 2300 || epsg == 2301 || epsg == 2302 ||
           epsg == 2303 || epsg == 2304 || epsg == 2305 || epsg == 2306 || epsg == 2307 || epsg == 2963 || epsg == 2985 || epsg == 2986 || epsg == 3052 ||
           epsg == 3053 || epsg == 3139 || epsg == 3144 || epsg == 3145 || epsg == 3173 || epsg == 3295 || epsg == 3993 || epsg == 4087 || epsg == 4088 ||
           epsg == 5017 || epsg == 5221 || epsg == 5224 || epsg == 5225 || epsg == 5514 || epsg == 5515 || epsg == 5516 || epsg == 5819 || epsg == 5820 ||
           epsg == 5821 || epsg == 6200 || epsg == 6201 || epsg == 6202 || epsg == 6244 || epsg == 6245 || epsg == 6246 || epsg == 6247 || epsg == 6248 ||
           epsg == 6249 || epsg == 6250 || epsg == 6251 || epsg == 6252 || epsg == 6253 || epsg == 6254 || epsg == 6255 || epsg == 6256 || epsg == 6257 ||
           epsg == 6258 || epsg == 6259 || epsg == 6260 || epsg == 6261 || epsg == 6262 || epsg == 6263 || epsg == 6264 || epsg == 6265 || epsg == 6266 ||
           epsg == 6267 || epsg == 6268 || epsg == 6269 || epsg == 6270 || epsg == 6271 || epsg == 6272 || epsg == 6273 || epsg == 6274 || epsg == 6275 ||
           epsg == 32600 || epsg == 32663 || epsg == 32700 )
        continue;

      if ( OSRImportFromEPSG( crs, epsg ) != OGRERR_NONE )
      {
        qDebug( "EPSG %d: not imported", epsg );
        continue;
      }

      char *wkt = nullptr;
      if ( OSRExportToWkt( crs, &wkt ) != OGRERR_NONE )
      {
        qWarning( "EPSG %d: not exported to WKT", epsg );
        continue;
      }

      wkts.insert( epsg, wkt );
      n++;

      OGRFree( wkt );
    }

    f.close();

    qDebug( "Loaded %d/%d from %s", n, l, filename.toUtf8().constData() );
  }

  OSRDestroySpatialReference( crs );

  return true;
}

int QgsCoordinateReferenceSystem::syncDb()
{
  QString dbFilePath = srsDbFilePath;
  syncDatumTransform( dbFilePath );

  int inserted = 0, updated = 0, deleted = 0, errors = 0;

  //qDebug( "Load srs db from: %s", QgsApplication::srsDbFilePath().toLocal8Bit().constData() );

  sqlite3 *database;
  if ( sqlite3_open( dbFilePath.toUtf8().constData(), &database ) != SQLITE_OK )
  {
 //   qCritical( "Could not open database: %s [%s]\n", QgsApplication::srsDbFilePath().toLocal8Bit().constData(), sqlite3_errmsg( database ) );
    return -1;
  }

  if ( sqlite3_exec( database, "BEGIN TRANSACTION", nullptr, nullptr, nullptr ) != SQLITE_OK )
  {
    //qCritical( "Could not begin transaction: %s [%s]\n", QgsApplication::srsDbFilePath().toLocal8Bit().constData(), sqlite3_errmsg( database ) );
    return -1;
  }

  // fix up database, if not done already //
  if ( sqlite3_exec( database, "alter table tbl_srs add noupdate boolean", nullptr, nullptr, nullptr ) == SQLITE_OK )
    ( void )sqlite3_exec( database, "update tbl_srs set noupdate=(auth_name='EPSG' and auth_id in (5513,5514,5221,2065,102067,4156,4818))", nullptr, 0, 0 );

  ( void )sqlite3_exec( database, "UPDATE tbl_srs SET srid=141001 WHERE srid=41001 AND auth_name='OSGEO' AND auth_id='41001'", nullptr, 0, 0 );

  OGRSpatialReferenceH crs = OSRNewSpatialReference( nullptr );
  const char *tail;
  sqlite3_stmt *select;
  char *errMsg = nullptr;

  QString proj4;
  QString sql;
  QHash<int, QString> wkts;
  loadIDs( wkts );
  loadWkts( wkts, "epsg.wkt" );

  qDebug( "%d WKTs loaded", wkts.count() );

  for ( QHash<int, QString>::const_iterator it = wkts.constBegin(); it != wkts.constEnd(); ++it )
  {
    QByteArray ba( it.value().toUtf8() );
    char *psz = ba.data();
    OGRErr ogrErr = OSRImportFromWkt( crs, &psz );
    if ( ogrErr != OGRERR_NONE )
      continue;

    if ( OSRExportToProj4( crs, &psz ) != OGRERR_NONE )
      continue;

    proj4 = psz;
    proj4 = proj4.trimmed();

    CPLFree( psz );

    if ( proj4.isEmpty() )
      continue;

    sql = QString( "SELECT parameters,noupdate FROM tbl_srs WHERE auth_name='EPSG' AND auth_id='%1'" ).arg( it.key() );
    if ( sqlite3_prepare( database, sql.toAscii(), sql.size(), &select, &tail ) != SQLITE_OK )
    {
      qCritical( "Could not prepare: %s [%s]\n", sql.toAscii().constData(), sqlite3_errmsg( database ) );
      continue;
    }

    QString srsProj4;
    if ( sqlite3_step( select ) == SQLITE_ROW )
    {
      srsProj4 = reinterpret_cast< const char * >( sqlite3_column_text( select, 0 ) );

      if ( QString::fromUtf8( reinterpret_cast< const char * >( sqlite3_column_text( select, 1 ) ) ).toInt() != 0 )
        continue;
    }

    sqlite3_finalize( select );

    if ( !srsProj4.isEmpty() )
    {
      if ( proj4 != srsProj4 )
      {
        errMsg = nullptr;
        sql = QString( "UPDATE tbl_srs SET parameters=%1 WHERE auth_name='EPSG' AND auth_id=%2" ).arg( quotedValue( proj4 ) ).arg( it.key() );

        if ( sqlite3_exec( database, sql.toUtf8(), nullptr, nullptr, &errMsg ) != SQLITE_OK )
        {
          qCritical( "Could not execute: %s [%s/%s]\n",
                     sql.toLocal8Bit().constData(),
                     sqlite3_errmsg( database ),
                     errMsg ? errMsg : "(unknown error)" );
          errors++;
        }
        else
        {
          updated++;
          //QgsDebugMsgLevel( QString( "SQL: %1\n OLD:%2\n NEW:%3" ).arg( sql, srsProj4, proj4 ), 3 );
        }
      }
    }
    else
    {
      QRegExp projRegExp( "\\+proj=(\\S+)" );
      if ( projRegExp.indexIn( proj4 ) < 0 )
      {
       // QgsDebugMsg( QString( "EPSG %1: no +proj argument found [%2]" ).arg( it.key() ).arg( proj4 ) );
        continue;
      }

      QRegExp ellipseRegExp( "\\+ellps=(\\S+)" );
      QString ellps;
      if ( ellipseRegExp.indexIn( proj4 ) >= 0 )
      {
        ellps = ellipseRegExp.cap( 1 );
      }

      QString name( OSRIsGeographic( crs ) ? OSRGetAttrValue( crs, "GEOCS", 0 ) : OSRGetAttrValue( crs, "PROJCS", 0 ) );
      if ( name.isEmpty() )
        name = QObject::tr( "Imported from GDAL" );

      sql = QString( "INSERT INTO tbl_srs(description,projection_acronym,ellipsoid_acronym,parameters,srid,auth_name,auth_id,is_geo,deprecated) VALUES (%1,%2,%3,%4,%5,'EPSG',%5,%6,0)" )
            .arg( quotedValue( name ),
                  quotedValue( projRegExp.cap( 1 ) ),
                  quotedValue( ellps ),
                  quotedValue( proj4 ) )
            .arg( it.key() )
            .arg( OSRIsGeographic( crs ) );

      errMsg = nullptr;
      if ( sqlite3_exec( database, sql.toUtf8(), nullptr, nullptr, &errMsg ) == SQLITE_OK )
      {
        inserted++;
      }
      else
      {
        qCritical( "Could not execute: %s [%s/%s]\n",
                   sql.toLocal8Bit().constData(),
                   sqlite3_errmsg( database ),
                   errMsg ? errMsg : "(unknown error)" );
        errors++;

        if ( errMsg )
          sqlite3_free( errMsg );
      }
    }
  }

  sql = "DELETE FROM tbl_srs WHERE auth_name='EPSG' AND NOT auth_id IN (";
  QString delim;
  QHash<int, QString>::const_iterator it = wkts.constBegin();
  for ( ; it != wkts.constEnd(); ++it )
  {
    sql += delim + QString::number( it.key() );
    delim = ',';
  }
  sql += ") AND NOT noupdate";

  if ( sqlite3_exec( database, sql.toUtf8(), nullptr, nullptr, nullptr ) == SQLITE_OK )
  {
    deleted = sqlite3_changes( database );
  }
  else
  {
    errors++;
    qCritical( "Could not execute: %s [%s]\n",
               sql.toLocal8Bit().constData(),
               sqlite3_errmsg( database ) );
  }

#if !defined(PJ_VERSION) || PJ_VERSION!=470
  sql = QString( "select auth_name,auth_id,parameters from tbl_srs WHERE auth_name<>'EPSG' AND NOT deprecated AND NOT noupdate" );
  if ( sqlite3_prepare( database, sql.toAscii(), sql.size(), &select, &tail ) == SQLITE_OK )
  {
    while ( sqlite3_step( select ) == SQLITE_ROW )
    {
      const char *auth_name = reinterpret_cast< const char * >( sqlite3_column_text( select, 0 ) );
      const char *auth_id   = reinterpret_cast< const char * >( sqlite3_column_text( select, 1 ) );
      const char *params    = reinterpret_cast< const char * >( sqlite3_column_text( select, 2 ) );

      QString input = QString( "+init=%1:%2" ).arg( QString( auth_name ).toLower(), auth_id );
      projPJ pj = pj_init_plus( input.toAscii() );
      if ( !pj )
      {
        input = QString( "+init=%1:%2" ).arg( QString( auth_name ).toUpper(), auth_id );
        pj = pj_init_plus( input.toAscii() );
      }

      if ( pj )
      {
        char *def = pj_get_def( pj, 0 );
        if ( def )
        {
          proj4 = def;
          pj_dalloc( def );

          input.prepend( ' ' ).append( ' ' );
          if ( proj4.startsWith( input ) )
          {
            proj4 = proj4.mid( input.size() );
            proj4 = proj4.trimmed();
          }

          if ( proj4 != params )
          {
            sql = QString( "UPDATE tbl_srs SET parameters=%1 WHERE auth_name=%2 AND auth_id=%3" )
                  .arg( quotedValue( proj4 ),
                        quotedValue( auth_name ),
                        quotedValue( auth_id ) );

            if ( sqlite3_exec( database, sql.toUtf8(), nullptr, nullptr, &errMsg ) == SQLITE_OK )
            {
              updated++;
              //QgsDebugMsgLevel( QString( "SQL: %1\n OLD:%2\n NEW:%3" ).arg( sql, params, proj4 ), 3 );
            }
            else
            {
              qCritical( "Could not execute: %s [%s/%s]\n",
                         sql.toLocal8Bit().constData(),
                         sqlite3_errmsg( database ),
                         errMsg ? errMsg : "(unknown error)" );
              errors++;
            }
          }
        }
        else
        {
        //  QgsDebugMsg( QString( "could not retrieve proj string for %1 from PROJ" ).arg( input ) );
        }
      }
      else
      {
        //QgsDebugMsgLevel( QString( "could not retrieve crs for %1 from PROJ" ).arg( input ), 3 );
      }

      pj_free( pj );
    }
  }
  else
  {
    errors++;
    qCritical( "Could not execute: %s [%s]\n",
               sql.toLocal8Bit().constData(),
               sqlite3_errmsg( database ) );
  }
#endif

  OSRDestroySpatialReference( crs );

  if ( sqlite3_exec( database, "COMMIT", nullptr, nullptr, nullptr ) != SQLITE_OK )
  {
    //qCritical( "Could not commit transaction: %s [%s]\n", QgsApplication::srsDbFilePath().toLocal8Bit().constData(), sqlite3_errmsg( database ) );
    return -1;
  }

  sqlite3_close( database );

  qWarning( "CRS update (inserted:%d updated:%d deleted:%d errors:%d)", inserted, updated, deleted, errors );

  if ( errors > 0 )
    return -errors;
  else
    return updated + inserted;
}

bool QgsCoordinateReferenceSystem::syncDatumTransform( const QString& dbPath )
{
  const char *filename = CSVFilename( "datum_shift.csv" );
  FILE *fp = VSIFOpen( filename, "rb" );
  if ( !fp )
  {
    return false;
  }

  char **fieldnames = CSVReadParseLine( fp );

  // "SEQ_KEY","COORD_OP_CODE","SOURCE_CRS_CODE","TARGET_CRS_CODE","REMARKS","COORD_OP_SCOPE","AREA_OF_USE_CODE","AREA_SOUTH_BOUND_LAT","AREA_NORTH_BOUND_LAT","AREA_WEST_BOUND_LON","AREA_EAST_BOUND_LON","SHOW_OPERATION","DEPRECATED","COORD_OP_METHOD_CODE","DX","DY","DZ","RX","RY","RZ","DS","PREFERRED"

  struct
  {
    const char *src;
    const char *dst;
    int idx;
  } map[] =
  {
    // { "SEQ_KEY", "", -1 },
    { "SOURCE_CRS_CODE", "source_crs_code", -1 },
    { "TARGET_CRS_CODE", "target_crs_code", -1 },
    { "REMARKS", "remarks", -1 },
    { "COORD_OP_SCOPE", "scope", -1 },
    { "AREA_OF_USE_CODE", "area_of_use_code", -1 },
    // { "AREA_SOUTH_BOUND_LAT", "", -1 },
    // { "AREA_NORTH_BOUND_LAT", "", -1 },
    // { "AREA_WEST_BOUND_LON", "", -1 },
    // { "AREA_EAST_BOUND_LON", "", -1 },
    // { "SHOW_OPERATION", "", -1 },
    { "DEPRECATED", "deprecated", -1 },
    { "COORD_OP_METHOD_CODE", "coord_op_method_code", -1 },
    { "DX", "p1", -1 },
    { "DY", "p2", -1 },
    { "DZ", "p3", -1 },
    { "RX", "p4", -1 },
    { "RY", "p5", -1 },
    { "RZ", "p6", -1 },
    { "DS", "p7", -1 },
    { "PREFERRED", "preferred", -1 },
    { "COORD_OP_CODE", "coord_op_code", -1 },
  };

  QString update = "UPDATE tbl_datum_transform SET ";
  QString insert, values;

  int n = CSLCount( fieldnames );

  int idxid = -1, idxrx = -1, idxry = -1, idxrz = -1, idxmcode = -1;
  for ( unsigned int i = 0; i < sizeof( map ) / sizeof( *map ); i++ )
  {
    bool last = i == sizeof( map ) / sizeof( *map ) - 1;

    map[i].idx = CSLFindString( fieldnames, map[i].src );
    if ( map[i].idx < 0 )
    {
      qWarning( "field %s not found", map[i].src );
      CSLDestroy( fieldnames );
      fclose( fp );
      return false;
    }

    if ( strcmp( map[i].src, "COORD_OP_CODE" ) == 0 )
      idxid = i;
    if ( strcmp( map[i].src, "RX" ) == 0 )
      idxrx = i;
    if ( strcmp( map[i].src, "RY" ) == 0 )
      idxry = i;
    if ( strcmp( map[i].src, "RZ" ) == 0 )
      idxrz = i;
    if ( strcmp( map[i].src, "COORD_OP_METHOD_CODE" ) == 0 )
      idxmcode = i;

    if ( i > 0 )
    {
      insert += ',';
      values += ',';

      if ( last )
      {
        update += " WHERE ";
      }
      else
      {
        update += ',';
      }
    }

    update += QString( "%1=%%2" ).arg( map[i].dst ).arg( i + 1 );

    insert += map[i].dst;
    values += QString( "%%1" ).arg( i + 1 );
  }

  insert = "INSERT INTO tbl_datum_transform(" + insert + ") VALUES (" + values + ')';

//  QgsDebugMsgLevel( QString( "insert:%1" ).arg( insert ), 4 );
  //QgsDebugMsgLevel( QString( "update:%1" ).arg( update ), 4 );

  CSLDestroy( fieldnames );

  Q_ASSERT( idxid >= 0 );
  Q_ASSERT( idxrx >= 0 );
  Q_ASSERT( idxry >= 0 );
  Q_ASSERT( idxrz >= 0 );

  sqlite3 *db;
  int openResult = sqlite3_open( dbPath.toUtf8().constData(), &db );
  if ( openResult != SQLITE_OK )
  {
    fclose( fp );
    return false;
  }

  if ( sqlite3_exec( db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr ) != SQLITE_OK )
  {
    //qCritical( "Could not begin transaction: %s [%s]\n", QgsApplication::srsDbFilePath().toLocal8Bit().constData(), sqlite3_errmsg( db ) );
    sqlite3_close( db );
    fclose( fp );
    return false;
  }

  QStringList v;
  v.reserve( sizeof( map ) / sizeof( *map ) );

  while ( !feof( fp ) )
  {
    char **values = CSVReadParseLine( fp );

    v.clear();

    if ( CSLCount( values ) < n )
    {
      qWarning( "Only %d columns", CSLCount( values ) );
      continue;
    }

    for ( unsigned int i = 0; i < sizeof( map ) / sizeof( *map ); i++ )
    {
      int idx = map[i].idx;
      Q_ASSERT( idx != -1 );
      Q_ASSERT( idx < n );
      v.insert( i, *values[ idx ] ? quotedValue( values[idx] ) : "NULL" );
    }

    //switch sign of rotation parameters. See http://trac.osgeo.org/proj/wiki/GenParms#towgs84-DatumtransformationtoWGS84
    if ( v.at( idxmcode ).compare( QLatin1String( "'9607'" ) ) == 0 )
    {
      v[ idxmcode ] = "'9606'";
      v[ idxrx ] = '\'' + qgsDoubleToString( -( v[ idxrx ].remove( '\'' ).toDouble() ) ) + '\'';
      v[ idxry ] = '\'' + qgsDoubleToString( -( v[ idxry ].remove( '\'' ).toDouble() ) ) + '\'';
      v[ idxrz ] = '\'' + qgsDoubleToString( -( v[ idxrz ].remove( '\'' ).toDouble() ) ) + '\'';
    }

    //entry already in db?
    sqlite3_stmt *stmt;
    QString cOpCode;
    QString sql = QString( "SELECT coord_op_code FROM tbl_datum_transform WHERE coord_op_code=%1" ).arg( v[ idxid ] );
    int prepareRes = sqlite3_prepare( db, sql.toAscii(), sql.size(), &stmt, nullptr );
    if ( prepareRes != SQLITE_OK )
      continue;

    if ( sqlite3_step( stmt ) == SQLITE_ROW )
    {
      cOpCode = reinterpret_cast< const char * >( sqlite3_column_text( stmt, 0 ) );
    }
    sqlite3_finalize( stmt );

    sql = cOpCode.isEmpty() ? insert : update;
    for ( int i = 0; i < v.size(); i++ )
    {
      sql = sql.arg( v[i] );
    }

    if ( sqlite3_exec( db, sql.toUtf8(), nullptr, nullptr, nullptr ) != SQLITE_OK )
    {
      qCritical( "SQL: %s", sql.toUtf8().constData() );
      qCritical( "Error: %s", sqlite3_errmsg( db ) );
    }
  }

  if ( sqlite3_exec( db, "COMMIT", nullptr, nullptr, nullptr ) != SQLITE_OK )
  {
    //qCritical( "Could not commit transaction: %s [%s]\n", QgsApplication::srsDbFilePath().toLocal8Bit().constData(), sqlite3_errmsg( db ) );
    return false;
  }

  sqlite3_close( db );
  return true;
}

QString QgsCoordinateReferenceSystem::geographicCRSAuthId() const
{
  if ( geographicFlag() )
  {
    return d->mAuthId;
  }
  else if ( d->mCRS )
  {
    return OSRGetAuthorityName( d->mCRS, "GEOGCS" ) + QLatin1String( ":" ) + OSRGetAuthorityCode( d->mCRS, "GEOGCS" );
  }
  else
  {
    return "";
  }
}

QStringList QgsCoordinateReferenceSystem::recentProjections()
{
  QStringList projections;

  // Read settings from persistent storage
  QSettings settings;
  projections = settings.value( "/UI/recentProjections" ).toStringList();
  /*** The reading (above) of internal id from persistent storage should be removed sometime in the future */
  /*** This is kept now for backwards compatibility */

  QStringList projectionsProj4  = settings.value( "/UI/recentProjectionsProj4" ).toStringList();
  QStringList projectionsAuthId = settings.value( "/UI/recentProjectionsAuthId" ).toStringList();
  if ( projectionsAuthId.size() >= projections.size() )
  {
    // We had saved state with AuthId and Proj4. Use that instead
    // to find out the crs id
    projections.clear();
    for ( int i = 0; i <  projectionsAuthId.size(); i++ )
    {
      // Create a crs from the EPSG
      QgsCoordinateReferenceSystem crs;
      crs.createFromOgcWmsCrs( projectionsAuthId.at( i ) );
      if ( ! crs.isValid() )
      {
        // Couldn't create from EPSG, try the Proj4 string instead
        if ( i >= projectionsProj4.size() || !crs.createFromProj4( projectionsProj4.at( i ) ) )
        {
          // No? Skip this entry
          continue;
        }
        //If the CRS can be created but do not correspond to a CRS in the database, skip it (for example a deleted custom CRS)
        if ( crs.srsid() == 0 )
        {
          continue;
        }
      }
      projections << QString::number( crs.srsid() );
    }
  }
  return projections;
}
