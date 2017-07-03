/*©mit**************************************************************************
*                                                                              *
* This file is part of FRIEND UNIFYING PLATFORM.                               *
* Copyright 2014-2017 Friend Software Labs AS                                  *
*                                                                              *
* Permission is hereby granted, free of charge, to any person obtaining a copy *
* of this software and associated documentation files (the "Software"), to     *
* deal in the Software without restriction, including without limitation the   *
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or  *
* sell copies of the Software, and to permit persons to whom the Software is   *
* furnished to do so, subject to the following conditions:                     *
*                                                                              *
* The above copyright notice and this permission notice shall be included in   *
* all copies or substantial portions of the Software.                          *
*                                                                              *
* This program is distributed in the hope that it will be useful,              *
* but WITHOUT ANY WARRANTY; without even the implied warranty of               *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                 *
* MIT License for more details.                                                *
*                                                                              *
*****************************************************************************©*/

/**
 *  @file
 *  AuthenticationModule body
 *
 *  @author PS (Pawel Stefanski)
 *  @date created on 01/2017
 */

#include <core/types.h>
#include "authmodule.h"
#include <util/log/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <system/systembase.h>

/**
 * create new AuthMod (AuthenticationModule)
 *
 * @param lsb pointer to SystemBase
 * @param path to authmod file
 * @param name name of required AuthenticationModule
 * @param version required version of AuthenticationModule
 * @return new AuthMod structure or NULL if error appear
 */
AuthMod *AuthModNew( void *lsb, const char *path, const char* name, long version )
{
	if( name == NULL )
	{
		FERROR("Cannot open login module with empty name!\n");
		return NULL;
	}
	
	if( !lsb )
	{
		FERROR("Cannot open login module with SysBase.library pointer!\n");
		//return NULL;
	}
		
	if( name == NULL )
	{
		FERROR("Name parameter is null\n");
		return NULL;
	}
	
	FBOOL loaded = FALSE;
	AuthMod *l = NULL;
	void *handle = NULL;
	long ( *GetVersion )( void );
	long ( *GetRevision )( void );
	//void * ( *libInit )( void * );

	char lmodpath[ 2048 ];
	
	snprintf( lmodpath, 2048, "%s%s", path, name );

	//
	// checking login module in provided path
	//
	
	// there is no need to multiply by sizeof(char)
	DEBUG( "[AuthMod] Current directory %s\n", lmodpath );

	if( ( handle = dlopen ( lmodpath, RTLD_NOW|RTLD_GLOBAL ) ) != NULL )
	{
		GetVersion = dlsym( handle, "GetVersion" );
		if( GetVersion != NULL )
		{
			if( GetVersion() >= version )
			{
				loaded = TRUE;
			}
			else
			{
				DEBUG( "[AuthMod] Version fail\n" );
			}
		}
		else
		{
			DEBUG( "[AuthMod] GetVersion fail\n" );
		}
		dlclose( handle );
	}
	else
	{
		DEBUG( "[AuthMod] Cannot open file\n" );
	}

	char* error = dlerror();
	if( error )
	{
		FERROR( "[AuthMod] Library error: %s  DYNAMIC LINK ERROR\n", error );
	}

	if( loaded == FALSE )
	{
		DEBUG( "[AuthMod] Couldn't find or open library %s\n", name );
		return NULL;
	}
	
	if( ( l = FCalloc( 1, sizeof( AuthMod ) ) ) != NULL )
	{
		DEBUG( "[AuthMod] Before lib init\n" );
		
		int blockAccountTimeout = 3600;
		int blockAccountAttempts = 3;
		Props *prop = NULL;
		
		SystemBase *sb = (SystemBase *) lsb;
		// Get a copy of the properties.library
		struct PropertiesLibrary *plib = ( struct PropertiesLibrary *)sb->LibraryPropertiesGet( sb );
		if( plib != NULL )
		{
			char *ptr = getenv("FRIEND_HOME");
			char *path = FCalloc( 1000, sizeof( char ) );
			
			if( ptr != NULL )
			{
				sprintf( path, "%scfg/cfg.ini", ptr );
			}
			
			DEBUG( "Opening config file: %s\n", path );
			
			prop = plib->Open( path );
			FFree( path );
			
			if( prop != NULL)
			{
				blockAccountTimeout = plib->ReadInt( prop, "Security:blocktimeout", 3600 );
				blockAccountAttempts = plib->ReadInt( prop, "Security:blockattempts", 3 );
			}
			else
			{
				FERROR( "Cannot open property file!\n" );
			}
			
			if( prop ) plib->Close( prop );
			
			sb->LibraryPropertiesDrop( sb, plib );
		}
		
		
		{
			l->sb = lsb;
		
			DEBUG( "[AuthMod] After init\n" );

			l->am_Handle = handle = dlopen ( lmodpath, RTLD_NOW|RTLD_GLOBAL );
			l->libInit = dlsym( l->am_Handle, "libInit" );
			l->GetRevision = dlsym( l->am_Handle, "GetRevision" );
			l->libClose = dlsym( l->am_Handle, "libClose" );
			l->libClose = dlsym ( l->am_Handle, "libClose" );
			l->GetVersion = dlsym ( l->am_Handle, "GetVersion" );
			l->GetRevision = dlsym( l->am_Handle, "GetRevision" );

			// user.library structure
			l->Authenticate = dlsym ( l->am_Handle, "Authenticate" );
			l->IsSessionValid = dlsym ( l->am_Handle, "IsSessionValid" );
			l->SetAttribute = dlsym ( l->am_Handle, "SetAttribute" );
			l->CheckPassword = dlsym( l->am_Handle, "CheckPassword" );
			l->UpdatePassword = dlsym( l->am_Handle, "UpdatePassword" );
			l->WebRequest = dlsym( l->am_Handle, "WebRequest" );
			l->Logout = dlsym( l->am_Handle, "Logout" );
			
			l->libInit( l, sb ) ;
	
			DEBUG( "[AuthMod] Completed initialization...\n" );
		}
		
		l->am_BlockAccountTimeout = blockAccountTimeout;
		l->am_BlockAccountAttempts = blockAccountAttempts;
	}
	
	return l;
}

/**
 * delete AuthMod (AuthenticationModule)
 *
 * @param lib pointer to AuthMod which will be removed
 */
void AuthModDelete( AuthMod *lib )
{
	if( lib == NULL ) return;
	DEBUG( "[AuthMod] Closing library %p\n", lib );

	lib->libClose( (void *)lib );

	if( lib->am_Handle != NULL )
	{
		dlclose( lib->am_Handle );
		lib->am_Handle = NULL;
	}
	FFree( lib );
	DEBUG( "[AuthMod] Lib closed memory free\n" );
}
