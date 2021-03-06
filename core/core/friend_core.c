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
/** @file
 *
 *  Friend Core
 *
 *  Friend Core is the root of the system
 *  in C and a message dispatch mechanism.
 *
 *  @author PS (Pawel Stefanski)
 *  @date pushed 19/10/2015
 */

#define _POSIX_SOURCE

#include <core/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#ifdef USE_SELECT

#else
#include <sys/epoll.h>
#endif
#include <arpa/inet.h>
#include "util/string.h"
#include "core/friend_core.h"
#include "network/socket.h"
#include "network/protocol_http.h"
#include <util/log/log.h>
#include <system/services/service_manager.h>
#include <util/buffered_string.h>
#include <util/http_string.h>
#include <openssl/rand.h>

#include <hardware/network.h>

#include <system/systembase.h>
#include <core/friendcore_manager.h>
#include <openssl/crypto.h>

//
//
//

extern void *FCM;			// FriendCoreManager
pthread_mutex_t maxthreadmut;

struct AcceptPair *DoAccept( Socket *sock );

static int nothreads = 0;					/// threads coutner @todo to rewrite
#define MAX_CALLHANDLER_THREADS 256			///< maximum number of simulatenous handlers

/**
 * Mutex buffer for ssl locking
 */

static pthread_mutex_t *ssl_mutex_buf = NULL;

/**
 * Static locking function.
 *
 * @param mode identifier of the mutex mode to use
 * @param n number of the mutex to use
 * @param file not used
 * @param line not used
 */

static void ssl_locking_function( int mode, int n, const char *file, int line )
{ 
    if( mode & CRYPTO_LOCK )
    {
        pthread_mutex_lock( &ssl_mutex_buf[ n ] );
    }
    else
    {
        pthread_mutex_unlock( &ssl_mutex_buf[ n ] );
    }
}

/**
 * Static ID function.
 *
 * @return function return thread ID as unsigned long
 */

static unsigned long ssl_id_function( void ) 
{
    return ( ( unsigned long )pthread_self() );
}

/**
 * Creates a new instance of Friend Core.
 *
 * @param ssl flag to indicate the use of ssl protocol
 * @param port communication port number
 * @param maxp maximum number of polls at the same time FL>PS ?
 * @param bufsiz buffer size
 * @param hostname FC host name
 * @param workers number of workers
 * @return pointer to the new instance of FriendCore
 * @return NULL in case of error
 */

FriendCoreInstance *FriendCoreNew( void *sb, FBOOL ssl, int port, int maxp, int bufsiz, char *hostname )
{
	LOG( FLOG_INFO, "[FriendCoreNew] Starting friend core\n" );
	
	// Static locks callbacks
	SSL_library_init();
	
	// Watch our threads
	// TODO: make an array and use one for each friend core! (if multiple)
	pthread_mutex_init( &maxthreadmut, NULL );
	
	// Static locks buffer
	ssl_mutex_buf = FCalloc( CRYPTO_num_locks(), sizeof( pthread_mutex_t ) );
	if( ssl_mutex_buf == NULL)
	{ 
		LOG( FLOG_PANIC, "[FriendCoreNew] Failed to allocate ssl mutex buffer.\n" );
		return NULL; 
	} 
	{
		int i; for( i = 0; i < CRYPTO_num_locks(); i++ )
		{ 
			pthread_mutex_init( &ssl_mutex_buf[ i ], NULL );
		}
	} 
	// Setup static locking.
	CRYPTO_set_locking_callback( ssl_locking_function );
	CRYPTO_set_id_callback( ssl_id_function );
	
	/*// Dynamic locking
	CRYPTO_set_dynlock_create_callback( ssl_dyn_create_function );
	CRYPTO_set_dynlock_lock_callback( ssl_dyn_lock_function );
	CRYPTO_set_dynlock_destroy_callback( ssl_dyn_destroy_function );*/
	
	OpenSSL_add_all_algorithms();
	
	// Load the error strings for SSL & CRYPTO APIs 
	SSL_load_error_strings();
	
	RAND_load_file( "/dev/urandom", 1024 );
	
	// FOR DEBUG PURPOSES! -ht
	_reads = 0;
	_writes = 0;
	_sockets = 0;
	
	FriendCoreInstance *fc = FCalloc( sizeof(FriendCoreInstance) , 1 );

	if( fc != NULL )
	{
		fc->fci_Port = port;
		fc->fci_MaxPoll = maxp;
		fc->fci_BufferSize = bufsiz;
		fc->fci_SSLEnabled = ssl;
		fc->fci_SB = sb;
		strncpy( fc->fci_IP, hostname, 256 );
	}
	else
	{
		LOG( FLOG_PANIC, "Cannot allocate memory for FriendCore instance\n");
		return NULL;
	}
	
	// Init listen mutex
	pthread_mutex_init( &fc->fci_ListenMutex, NULL );
	
	LOG( FLOG_INFO,"[FriendCore] WorkerManager started\n");
	
	return fc;
}

/**
 * Stops and destroys an instance of Friend Core.
 *
 * @param fc pointer to Friend Core instance to destroy
 */

void FriendCoreShutdown( FriendCoreInstance* fc )
{	
	// Clear watchdog
	pthread_mutex_destroy( &maxthreadmut );

	DEBUG("[FriendCoreShutdown] On exit, we have %d idle threads.\n", nothreads );

	while( fc->fci_Closed != TRUE )
	{
		LOG( FLOG_INFO, "[FriendCoreShutdown] Waiting for close\n" );
		usleep( 5000 );
	}
	
	if( ssl_mutex_buf != NULL )
	{
		FFree( ssl_mutex_buf );
		ssl_mutex_buf = NULL;
	}
	
	// Destroy listen mutex
	DEBUG("[FriendCoreShutdown] Waiting for listen mutex\n" );
	if( pthread_mutex_lock( &fc->fci_ListenMutex ) == 0 )
	{
		pthread_mutex_unlock( &fc->fci_ListenMutex );
		pthread_mutex_destroy( &fc->fci_ListenMutex );
	}
	
	FFree( fc );
	
	LOG( FLOG_INFO,"FriendCore shutdown!\n");
}

//
// Handy string divider macro
//

static char *dividerStr = "\r\n\r\n";

//
//
//

#define NO_WORKERS

//
// Decrease number of threads
//

static void DecreaseThreads()
{
	if( pthread_mutex_lock( &maxthreadmut ) == 0 )
	{
		nothreads--;
#ifdef __DEBUG
		//LOG( FLOG_DEBUG,"<<<<<<<<<<<<<<<< %d %d\n", pthread_self(), nothreads );
#endif
		pthread_mutex_unlock( &maxthreadmut );
	}
}

static void IncreaseThreads()
{
	while( nothreads >= MAX_CALLHANDLER_THREADS )
	{
		usleep( 500 );
	}
	if( pthread_mutex_lock( &maxthreadmut ) == 0 )
	{
		nothreads++;
#ifdef __DEBUG
		//LOG( FLOG_DEBUG,">>>>>>>>>>>>>>>> %d %d\n", pthread_self(), nothreads );
#endif
		pthread_mutex_unlock( &maxthreadmut );
	}
}

//
// Current Friend Core instance
//

struct FriendCoreInstance *fci;

/**
 * Sends a quit message
 *
 * @param signal
 */
void SignalHandler( int signal )
{
	DEBUG("[FriendCoreShutdown] Quit signal sent\n");
#ifdef USE_SELECT
	
#endif

	FriendCoreManagerShutdown( FCM );
}

/**
 * Thread to handle incoming connections!!!
 */
struct fcThreadInstance 
{ 
	FriendCoreInstance *fc;
	pthread_t thread;
	struct epoll_event *event;
	Socket *sock;
	
	// Incoming from accept
	struct AcceptPair *acceptPair;
};

//#define USE_PTHREAD
#define USE_WORKERS

/**
 * Accepts a message to a Friend Core instance
 *
 * @param fcv pointer to thread instance of Friend Core
 * @return NULL
 */

void FriendCoreAccept( void *fcv )
{
#ifdef USE_PTHREAD
	pthread_detach( pthread_self() );
#endif
	
	IncreaseThreads();
	
	struct fcThreadInstance *th = ( struct fcThreadInstance *)fcv;
	
	FriendCoreInstance *fc = ( FriendCoreInstance * )th->fc;

	int incomingC = 0;
	
	// Get incoming
	Socket *incoming = NULL;
	int error = 0;
	
	// Get incoming socket

	incoming = SocketAcceptPair( fc->fci_Sockets, th->acceptPair );

	// We got incoming!
	if( incoming != NULL )
	{
		// Add instance reference
		incoming->s_Data = fc;
	#ifdef USE_SELECT
	
	#else
		pthread_mutex_lock( &incoming->mutex );
		/// Add to epoll
		// TODO: Check return of epoll ctl
		struct epoll_event event;
		event.data.fd = incoming->fd;
		event.data.ptr = incoming;
		event.events = EPOLLIN | EPOLLET; // old way | EPOLLET | EPOLLHUP | EPOLLRDHUP;
		error = epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_ADD, incoming->fd, &event );
	
		DEBUG("[FriendCoreAccept] Event added, shutdown %d error %d fd %d\n", fci->fci_Shutdown, error, incoming->fd  );
	
		pthread_mutex_unlock( &incoming->mutex );
	#endif // USE_SELECT
	
		if( fc->fci_Shutdown == TRUE )
		{
			shutdown( th->acceptPair->fd, SHUT_RDWR );
			close( th->acceptPair->fd );
		}
	}
	
	// Don't need these anymore
	if( th != NULL )
	{
		if( th->acceptPair != NULL )
		{
			FFree( th->acceptPair );
			th->acceptPair = NULL;
		}
		FFree( th );
		th = NULL;
	}

	// This is an in-thread threadcount decreasing, spawned by
	// code in FriendCoreAcceptPhase1 (further below in this file)
	
	
DecreaseThreads();

#ifdef USE_PTHREAD
	pthread_exit( 0 );
#endif
	return;
}

/**
 * Processes phase 1 of acceptation process.
 *
 * @param d pointer to Friend Core instance
 * @return unintialised pointer to a void
 */
void FriendCoreAcceptPhase1( void *d )
{
#ifdef USE_PTHREAD
	pthread_detach( pthread_self() );
#endif
	IncreaseThreads();

	struct fcThreadInstance *pre = ( struct fcThreadInstance *)d;
	
	// Ready our accept pair
	struct AcceptPair *p = NULL;
	
	// Run accept() and return an accept pair
	for( ; ( p = DoAccept( pre->fc->fci_Sockets ) ) != NULL ; )
	{
		// Shutting down
		if( pre->fc->fci_Shutdown == TRUE )
		{
			shutdown( p->fd, SHUT_RDWR );
			close( p->fd );
			FFree( p );
			break;
		}
		// Not shutting down
		else
		{
			// Add the new SSL accept thread:
			// We have a notification on the listening socket, which means one or more incoming connections.
		
			if( p != NULL )
			{
				struct fcThreadInstance *idata = FCalloc( 1, sizeof( struct fcThreadInstance ) );
				if( idata != NULL )
				{
					idata->fc = pre->fc;
					idata->acceptPair = p;
			
#ifdef USE_PTHREAD
					memset( &idata->thread, 0, sizeof( pthread_t ) );
					// Multithread mode
					if( pthread_create( &idata->thread, NULL, &FriendCoreAccept, ( void *)idata ) != 0 )
					{
						FFree( idata );
						// Clean up accept pair
						shutdown( p->fd, SHUT_RDWR );
						close( p->fd );
						FFree( p );
					}
					else
					{
	
					}
#else
#ifdef USE_WORKERS
					SystemBase *locsb = (SystemBase *)idata->fc->fci_SB;
					WorkerManagerRun( locsb->sl_WorkerManager,  FriendCoreAccept, idata, NULL );
#else
					int pid = fork();
				
					if( pid < 0 )
					{
						FERROR("Cannot create fork %ld \n", pid );
					}
					else if( pid == 0 )
					{
						FriendCoreAccept( idata );
					}
					else
					{
						INFO("Parent %ld \n", pid );
					}
#endif
#endif
				}
				else
				{
					FERROR("[FriendCoreAcceptPhase1] Cannot allocate memory for Thread\n");
				}
			}
		}
		//pthread_yield();
	}
	
	if( pre != NULL )
	{
		FFree( pre ); // <- remove thread instance
		pre = NULL;
	}

	DecreaseThreads();

#ifdef USE_PTHREAD
	pthread_exit( 0 );
#endif
	return;
}


/**
 * Processes Friend Core messages
 *
 * @param fcv pointer to thread instance of Friend Core
 * @return NULL
 */
void *FriendCoreProcess__httponthefly( void *fcv )
{
#ifdef USE_PTHREAD
	pthread_detach( pthread_self() );
#endif
	
	IncreaseThreads();
	
	DEBUG("[FriendCoreProcess] Before while\n");
	
	struct fcThreadInstance *th = ( struct fcThreadInstance *)fcv;
	
	// Get socket
	if( th->sock == NULL )
	{
		DecreaseThreads();
		FFree( th );
#ifdef USE_PTHREAD
		pthread_exit( 0 );
#endif
		return NULL;
	}
	
	// First pass header, second, data
	int pass = 0, bodyLength = 0, prevBufSize = 0, preroll = 0, stopReading = 0;
	
	// Often used
	int partialDivider = 0, foundDivider = 0, y = 0, x = 0, yc = 0, yy = 0;
	char findDivider[ 5 ];
	char *std_Buffer = FCalloc( HTTP_READ_BUFFER_DATA_SIZE_ALLOC, sizeof( char ) );
	if( std_Buffer != NULL )
	{
		HttpString *resultString = HttpStringNew( HTTP_READ_BUFFER_DATA_SIZE );
		
		int res = 0;
		int requestSize = 0;
		int timesRead = 0;
		FBOOL everythingReaded = FALSE;
		FBOOL parseOnlyHeader = FALSE;
		
		DEBUG("[FriendCoreProcess] before process ptr %p fd %d\n", th->sock, th->sock->fd );

		Http *request = NULL;
		
		while( TRUE )
		{
			if( ( res = SocketRead( th->sock, std_Buffer, HTTP_READ_BUFFER_DATA_SIZE, pass ) ) > 0 )
			{
				char stdBufChr = std_Buffer[ res - 1 ];
				requestSize += res;
				timesRead++;
				
				HttpStringAdd( resultString, std_Buffer, res );

				if( ( stdBufChr == '\r' ||  stdBufChr == '\n' ) )
				{
					if( res < HTTP_READ_BUFFER_DATA_SIZE )
					{
						everythingReaded = TRUE;

						// we have whole message, no need to read more
						//break;
					}
				}
				// enough to parse header
				else if( strstr( std_Buffer, dividerStr ) != NULL )
				{
					parseOnlyHeader = TRUE;
				}
				
				if( everythingReaded == TRUE || parseOnlyHeader == TRUE )
				{
					request = ( Http *)th->sock->data;
					if( request == NULL )
					{
						request = HttpNew( );
						request->timestamp = time( NULL );
						th->sock->data = ( void* )request;
						resultString->ht_Reqest = request;
					}
					
					if( request != NULL && request->gotHeader == FALSE )
					{
						int result = HttpParseHeader( request, resultString->ht_Buffer, resultString->ht_Size + 1 );
						if( result == 1 )
						{
							//char *content = HttpGetHeader( request, "content-length", 0 );
							//char *content = HttpGetHeaderFromTable( request, HTTP_HEADER_CONTENT_LENGTH );
							
							if( request->h_ContentLength > 0 )
							{
								//char *divider = strstr( content, dividerStr );
								char *divider = strstr( resultString->ht_Buffer, dividerStr );

								// No divider?
								if( !divider )
								{
									//prevBufSize += count;
									break;
								}
								//char *tmpval = StringDuplicateEOL( content );
								//if( tmpval != NULL )
								{
									bodyLength = request->h_ContentLength;//atoi( content );
									//FFree( tmpval );
								}
								//bodyLength = atoi( content );
								int headerLength = divider - resultString->ht_Buffer + 4;

								if( bodyLength == 124 )
								{
									//abort( );
								}
								// We have enough data and are ready for reading the body
								if( requestSize >= ( headerLength + bodyLength ) )
								{
									everythingReaded = TRUE;
								}
								
							}
							else
							{
								FERROR("content = NULL\n");
							}
						}
						else
						{
							FERROR("Error during parsing header %d\n", result );
						}
						
						if( bodyLength == 0 )
						{
							break;
						}
						request->gotHeader = TRUE;
					}
					
					if( everythingReaded == TRUE )
					{
						
						break;
					}
				}
				
				//TEST
				if( timesRead > 2 )
				{
					break;
				}
			}
			else
			{
				break;
			}
		}
		
		if( everythingReaded == TRUE )
		{
			// Free up
			if( resultString->ht_Buffer != NULL )
			{
				if( resultString->ht_Size > 0 )
				{
					// Process data
					//LOG( FLOG_DEBUG, "We received this (%d bytes): >>%s<<\n", resultString->bs_Size, resultString->bs_Buffer);
					Http *resp = ProtocolHttp( th->sock, resultString->ht_Buffer, resultString->ht_Size );
					
					// -1 is special case, the response already was sent and cleaned up
					if( resp != NULL )//&& resp != -1 )
					{
						if( resp->h_WriteType == FREE_ONLY )
						{
							HttpFree( resp );
						}
						else
						{
							HttpWriteAndFree( resp, th->sock );
						}
					}
				}
				// No data, just free
				else
				{
					// Ask for more info
					DEBUG( "[FriendCoreProcess] No buffer to write, so just free!\n" );
				}
				HttpStringDelete( resultString );
			}
			else
			{
				// Ask for more info
				DEBUG( "[FriendCoreProcess] No buffer to write!\n" );
			}
		}
		else
		{
			HttpStringDelete( resultString );
			if( request != NULL )
			{
				HttpFree( request );
			}
		}

		// Free the pair
		if( th != NULL )
		{
			FFree( th );
		}
		
		// Free up buffers
		FFree( std_Buffer );
	}
	
	SocketClose( th->sock );
	
	// No more threads
	DecreaseThreads();
#ifdef USE_PTHREAD
	pthread_exit( 0 );
#endif
	return NULL;
}





void FriendCoreProcess( void *fcv )
{
#ifdef USE_PTHREAD
	pthread_detach( pthread_self() );
#endif	
	IncreaseThreads();
	
	if( fcv == NULL )
	{
		DecreaseThreads();
#ifdef USE_PTHREAD
		pthread_exit( 0 );
#endif
		return;
	}
	
	struct fcThreadInstance *th = ( struct fcThreadInstance *)fcv;

	if( th->sock == NULL )
	{
		DecreaseThreads();
		FFree( th );
#ifdef USE_PTHREAD
		pthread_exit( 0 );
#endif
		return;
	}

	// Threadcounter done ------------------------------------------------------
	
	// Let's go!
	
	// First pass header, second, data
	int pass = 0, bodyLength = 0, prevBufSize = 0, preroll = 0, stopReading = 0;

	// Often used
	int partialDivider = 0, foundDivider = 0, y = 0;
	char findDivider[ 5 ];
	
	int bufferSize = HTTP_READ_BUFFER_DATA_SIZE;
	int bufferSizeAlloc = HTTP_READ_BUFFER_DATA_SIZE_ALLOC;

	char *locBuffer = FCalloc( bufferSizeAlloc, sizeof( char ) );
	if( locBuffer != NULL )
	{
		BufString *resultString = BufStringNewSize( bufferSizeAlloc << 1 );

		for( ; pass < 2; pass++ )
		{
			// No bodylength? Fuck it
			if( pass == 1 && ( !bodyLength || stopReading ) )
			{
				//FERROR( "We have Bodylength: %s, Stopreading: %s\n", !bodyLength ? "no body" : "have body", stopReading ? "stop" : "continue" );
				break;
			}

			int count = preroll, res = 0, joints = 0, methodGet = 0;

			// We must find divider!
			// TODO: before there was table of pointers and sign was placed there
			memset( findDivider, 0, 5 );

			char *stdBufChr = NULL;
			partialDivider = 0, foundDivider = 0, y = res - 1;

			for( ; ; )
			{
				// Increase the buffer for files!
				if( count != 0 && bufferSize == HTTP_READ_BUFFER_DATA_SIZE )
				{
					bufferSize = 1048576; // Bit faster, bit greedier
					bufferSizeAlloc = 1048608; // buffersize + 32
					FFree( locBuffer ); locBuffer = FCalloc( bufferSizeAlloc, sizeof( char ) );
				}
				
				if( ( res = SocketRead( th->sock, locBuffer, bufferSize, pass ) ) > 0 )
				{
					BufStringAddSize( resultString, locBuffer, res );

					if( pass == 0 && partialDivider != 0 )
					{
						stdBufChr = strstr( "\r", locBuffer );
						if( stdBufChr == NULL ) stdBufChr = strstr( "\n", locBuffer );
						if( stdBufChr != NULL )
						{
							findDivider[ partialDivider++ ] = stdBufChr[0];
							if( partialDivider > 3 )
							{
								findDivider[ 4 ] = '\0';
							}
						}
						else partialDivider = 0;
					}

					// How much data did we read?
					count += res;
					joints++;

					// Break get posts after header
					if( pass == 0 )
					{
						stdBufChr = locBuffer + res - 1;

						// If we found a divider!
						if( partialDivider >= 4 || strstr( locBuffer, dividerStr ) )
						{
							// We have a divider! Great!
							break;
						}
						// Does it end with this? Perhaps it's a partial divider
						else if( stdBufChr[0] == '\r' ||  stdBufChr[0] == '\n' )
						{
							memcpy( findDivider, locBuffer + res - 5, 5 );
							partialDivider = 5;
						}
					}
					// Or in second pass, body length
					else if( pass == 1 && count >= bodyLength )
					{
						// remove preroll to get correct read bytes
						count -= preroll;
						DEBUG( "[FriendCoreProcess] Fixing preroll %d\n", preroll );
						break;
					}
				}
				// No data?
				else break;
			}

			if( count > 0 )
			{
				// Already now parse header to receive other data
				if( pass == 0 )
				{
					Http *request = ( Http *)th->sock->data;
					int result = 0;
					char *content = NULL;
					{
						if( request == NULL )
						{
							request = HttpNew( );
							request->timestamp = time( NULL );
							th->sock->data = ( void* )request;
						}
						
						request->h_ShutdownPtr = &(th->fc->fci_Shutdown);

						request->h_Socket = th->sock;
						result = HttpParseHeader( request, resultString->bs_Buffer, resultString->bs_Size + 1 );
						request->gotHeader = TRUE;
						
						content = HttpGetHeaderFromTable( request, HTTP_HEADER_CONTENT_LENGTH );
					}

					// If we have content, then parse it
					if( request->h_ContentLength > 0 )
					{
						//rchar *divider = strstr( resultString->bs_Buffer, dividerStr );
						char *divider = strstr( content, dividerStr );
						
						// No divider?
						if( !divider )
						{
							prevBufSize += count;
							break;
						}

						bodyLength = request->h_ContentLength;//atoi( content );
						
						// We have enough data and are ready for reading the body
						int headerLength = divider - resultString->bs_Buffer + 4;
						if( count > headerLength )
						{
							prevBufSize += count;
							preroll = count - headerLength;
							stopReading = count >= ( headerLength + bodyLength );
							continue;
						}
					}
					// Need only one pass
					else
					{
						// No content length
						prevBufSize += count;
						break;
					}
					// Determine if we need pass 2
				}
				// For next pass
				prevBufSize += count;
			}
		}	// for pass 0 < 2
	
		//DEBUG( "[FriendCoreProcess] Exited headers loop. Now freeing up.\n" );

		// Free up
		if( resultString != NULL )
		{
			if( resultString->bs_Buffer != NULL )
			{
				if( resultString->bs_Size > 0 )
				{
					// Process data
					//LOG( FLOG_DEBUG, "We received this (%d bytes): >>%s<<\n", resultString->bs_Size, resultString->bs_Buffer);
					Http *resp = ProtocolHttp( th->sock, resultString->bs_Buffer, resultString->bs_Size );

					if( resp != NULL )
					{
						if( resp->h_WriteType == FREE_ONLY )
						{
							HttpFree( resp );
						}
						else
						{
							HttpWriteAndFree( resp, th->sock );
						}
					}
				}
				// No data, just free
				else
				{
					// Ask for more info
					DEBUG( "[FriendCoreProcess] No buffer to write, so just free!\n" );
				}
			}
			BufStringDelete( resultString );
		}
		else
		{
			// Ask for more info
			DEBUG( "[FriendCoreProcess] No buffer to write!\n" );
		}

		// Free up buffers
		FFree( locBuffer );
	}

	SocketClose( th->sock );
	
	// Free the pair
	if( th != NULL )
	{
		FFree( th );
		th = NULL;
	}
	
	// No more threads
	DecreaseThreads();
#ifdef USE_PTHREAD
	pthread_exit( 0 );
#endif
	return;
}

/**
 * Return an accept pair from accept
 *
 * @param sock pointer to associated socket
 * @return accepted pair
 * @return NULL in case of failure
 */
struct AcceptPair *DoAccept( Socket *sock )
{
	// Accept
	struct sockaddr_in6 client;
	socklen_t clientLen = sizeof( client );
	
	if( sock == NULL )
	{
		return NULL;
	}
	
	int fd = accept( sock->fd, ( struct sockaddr* )&client, &clientLen );
	
	if( fd == -1 ) 
	{
		// Get some info about failure..
		switch( errno )
		{
			case EAGAIN:
				break;
			case EBADF:
				DEBUG( "[AcceptPair] The socket argument is not a valid file descriptor.\n" );
				break;
			case ECONNABORTED:
				DEBUG( "[AcceptPair] A connection has been aborted.\n" );
				break;
			case EINTR:
				DEBUG( "[AcceptPair] The accept() function was interrupted by a signal that was caught before a valid connection arrived.\n" );
				break;
			case EINVAL:
				DEBUG( "[AcceptPair] The socket is not accepting connections.\n" );
				break;
			case ENFILE:
				DEBUG( "[AcceptPair] The maximum number of file descriptors in the system are already open.\n" );
				break;
			case ENOTSOCK:
				DEBUG( "[AcceptPair] The socket argument does not refer to a socket.\n" );
				break;
			case EOPNOTSUPP:
				DEBUG( "[AcceptPair] The socket type of the specified socket does not support accepting connections.\n" );
				break;
			default: 
				DEBUG("[AcceptPair] Accept return bad fd\n");
				break;
		}
		return NULL;
	}
	
	// Create socket object
	int prerr = getpeername( fd, (struct sockaddr *) &client, &clientLen );
	if( prerr == -1 )
	{
		close( fd );
		return NULL;
	}

	// Return copy
	struct AcceptPair *p = FCalloc( 1, sizeof( struct AcceptPair ) );
	if( p != NULL )
	{
		p->fd = fd;
		memcpy( &p->client, &client, sizeof( struct sockaddr_in6 ) );
	}

	return p;
}

#ifdef USE_SELECT
/**
 * Polls all Friend Core messages via Select().
 *
 * This is the main loop of the Friend Core system
 *
 * @param fc pointer to Friend Core instance to poll
 */
inline void FriendCoreSelect( FriendCoreInstance* fc )
{
	// add communication ReadCommPipe		
	//signal(SIGINT, test);
	// Handle signals and block while going on!
	struct sigaction setup_action;
	sigset_t block_mask, curmask; 
	sigemptyset( &block_mask );
	// Block other terminal-generated signals while handler runs.
	sigaddset( &block_mask, SIGINT );
	setup_action.sa_handler = SignalHandler;
#ifndef CYGWIN_BUILD
	setup_action.sa_restorer = NULL;
#endif
	setup_action.sa_mask = block_mask;
	setup_action.sa_flags = 0;
	sigaction( SIGINT, &setup_action, NULL );
	sigprocmask( SIG_SETMASK, NULL, &curmask );
	fci = fc;
	
	int pipefds[2] = {};
	pipe( pipefds );	
	fc->fci_ReadCorePipe = pipefds[ 0 ]; fc->fci_WriteCorePipe = pipefds[ 1 ];
	
	// Read buffer
	char buffer[ fc->fci_BufferSize ];
	int maxd = fc->fci_Sockets->fd;
	if( fc->fci_ReadCorePipe > maxd )
	{
		maxd = fc->fci_ReadCorePipe;
	}
	
	fd_set readfds;
	
	SocketSetBlocking( fc->fci_Sockets, TRUE );
	
	// All incoming network events go through here
	while( !fc->fci_Shutdown )
	{
		FD_ZERO( &readfds );
		
		FD_SET( fc->fci_Sockets->fd, &readfds );
		FD_SET( fc->fci_ReadCorePipe, &readfds );
		
		DEBUG("[FriendCoreSelect] Before select, maxd %d  pipe %d socket %d\n", maxd, fc->fci_ReadCorePipe, fc->fci_Sockets->fd );
		
		int activity = select( maxd+1, &readfds, NULL, NULL, NULL );
		
		DEBUG("[FriendCoreSelect] After select\n");
		
		if( ( activity < 0 ) && ( errno != EINTR ) )
		{
			DEBUG("[FriendCoreSelect] Select error\n");
		}
		
		if( FD_ISSET( fc->fci_ReadCorePipe, &readfds ) )
		{
			DEBUG("[FriendCoreSelect] Received from PIPE\n");
			// read all bytes from read end of pipe
			char ch;
			int result = 1;
			
			DEBUG("[FriendCoreSelect] FC Read from pipe!\n");
			
			while( result > 0 )
			{
				result = read( fc->fci_ReadCorePipe, &ch, 1 );
				DEBUG("[FriendCoreSelect] FC Read from pipe %c\n", ch );
				if( ch == 'q' )
				{
					fc->fci_Shutdown = TRUE;
					DEBUG("[FriendCoreSelect] FC Closing. Current reads: %d, writes: %d!\n", _reads, _writes );
					break;
				}
			}
			
			if( fc->fci_Shutdown == TRUE )
			{
				LOG( FLOG_INFO, "[FriendCoreSelect] Core shutdown in porgress\n");
				break;
			}
		}
		
		if( FD_ISSET( fc->fci_Sockets->fd, &readfds ) )
		{
			DEBUG("[FriendCoreSelect] Sockets received\n");
			
			FD_CLR( fc->fci_Sockets->fd, &readfds );
			
			Socket *sock = SocketAccept( fc->fci_Sockets );
			
			if( sock != NULL )
			{
				SocketSetBlocking( sock, TRUE );
			
				// go on and accept on the socket
				struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
				if( pre != NULL )
				{
					pre->fc = fc;
					pre->sock = sock;
					if( pthread_create( &pre->thread, NULL, &FriendCoreProcess, ( void *)pre ) != 0 )
					{
						FFree( pre );
					}
				}
			}
		}
		
		
	}	// shutdown
}
#else
/**
 * Polls all Friend Core messages.
 *
 * This is the main loop of the Friend Core system
 *
 * @param fc pointer to Friend Core instance to poll
 */
inline void FriendCoreEpoll( FriendCoreInstance* fc )
{
	int eventCount = 0;
	int retval;
	int i, ii;
	struct epoll_event *currentEvent;
	struct epoll_event *events = FCalloc( fc->fci_MaxPoll, sizeof( struct epoll_event ) );
	ssize_t count;
	Socket *sock = NULL;
	
	// Read buffer
	char buffer[ fc->fci_BufferSize ];
	
	// add communication ReadCommPipe		
	int pipefds[2] = {}; struct epoll_event piev = { 0 };	
	pipe( pipefds );	
	fc->fci_ReadCorePipe = pipefds[ 0 ]; fc->fci_WriteCorePipe = pipefds[ 1 ];
	// add the read end to the epoll
	piev.events = EPOLLIN; piev.data.fd = fc->fci_ReadCorePipe;
	int err = epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_ADD, fc->fci_ReadCorePipe, &piev );
	if( err != 0 )
	{
		LOG( FLOG_PANIC,"Cannot add main event %d\n", err );
	}

	// Handle signals and block while going on!
	struct sigaction setup_action;
	sigset_t block_mask, curmask; 
	sigemptyset( &block_mask );
	// Block other terminal-generated signals while handler runs.
	sigaddset( &block_mask, SIGINT );
	setup_action.sa_handler = SignalHandler;
	setup_action.sa_restorer = NULL;
	setup_action.sa_mask = block_mask;
	setup_action.sa_flags = 0;
	sigaction( SIGINT, &setup_action, NULL );
	sigprocmask( SIG_SETMASK, NULL, &curmask );
	fci = fc;

	// All incoming network events go through here
	while( !fc->fci_Shutdown )
	{
		// Wait for something to happen on any of the sockets we're listening on
		
		eventCount = epoll_pwait( fc->fci_Epollfd, events, fc->fci_MaxPoll, -1, &curmask );

		for( i = 0; i < eventCount; i++ )
		{
			currentEvent = &events[i];
			Socket *sock = ( Socket *)currentEvent->data.ptr;
			
			// Ok, we have a problem with our connection
			if( 
				( ( currentEvent->events & EPOLLERR ) ||
				( currentEvent->events & EPOLLRDHUP ) ||
				( currentEvent->events & EPOLLHUP ) ) || 
				!( currentEvent->events & EPOLLIN ) 
			)
			{
				if( currentEvent->data.fd == fc->fci_Sockets->fd )
				{
					// Oups! We have gone away!
					DEBUG( "[FriendCoreEpoll] Socket went away!\n" );
					break;
				}
								
				// Remove it
				LOG( FLOG_ERROR, "[FriendCoreEpoll] Socket had errors.\n" );
				if( sock != NULL )
				{
					DEBUG("[FriendCoreEpoll] FD %d\n", sock->fd );
					epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_DEL, sock->fd, NULL );
					SocketClose( sock );
				}
			}
			else if( currentEvent->events & EPOLLWAKEUP )
			{
				DEBUG( "[FriendCoreEpoll] Wake up!\n" );
			}
			// First handle pipe messages
			else if( currentEvent->data.fd == fc->fci_ReadCorePipe )
			{
				// read all bytes from read end of pipe
				char ch;
				int result = 1;
					
				DEBUG("[FriendCoreEpoll] FC Reads from pipe!\n");
				
				while( result > 0 )
				{
					result = read( fc->fci_ReadCorePipe, &ch, 1 );
					if( ch == 'q' )
					{
						fc->fci_Shutdown = TRUE;
						DEBUG("[FriendCoreEpoll] FC Closing all socket connections. Current reads: %d, writes: %d!\n", _reads, _writes );
						break;
					}
				}
				
				if( fc->fci_Shutdown == TRUE )
				{
					LOG( FLOG_INFO, "[FriendCoreEpoll] Core shutdown in porgress\n");
					break;
				}
			}
			// Accept incoming connections
			else if( currentEvent->data.fd == fc->fci_Sockets->fd && !fc->fci_Shutdown )
			{	
				// Setup for reading
				struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
				if( pre != NULL )
				{
					pre->fc = fc;
					// TODO: Make sure we keep the number of threads under the limit
					
#ifdef USE_PTHREAD
					if( pthread_create( &pre->thread, NULL, &FriendCoreAcceptPhase1, ( void *)pre ) != 0 )
					{
						FFree( pre );
					}
#else
#ifdef USE_WORKERS
					SystemBase *locsb = (SystemBase *)fc->fci_SB;
					WorkerManagerRun( locsb->sl_WorkerManager,  FriendCoreAcceptPhase1, pre, NULL );
					//WorkerManagerRun( fc->fci_WorkerManager,  FriendCoreAcceptPhase1, pre );
#else
					int pid;
					pid = fork();
					
					if( pid < 0 )
					{
						FERROR("FORK fail\n");
					}
					else if( pid == 0 )
					{
						FriendCoreAcceptPhase1( pre );
					}
					else
					{
						INFO("Fork done\n");
					}
#endif
#endif
				}
			}
			// Get event that are incoming!
			else if( currentEvent->events & EPOLLIN )
			{
				pthread_mutex_lock( &sock->mutex );
				epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_DEL, sock->fd, NULL );
				pthread_mutex_unlock( &sock->mutex );
				
				// Process

				if( !fc->fci_Shutdown )
				{
					struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
					if( pre != NULL )
					{
						pre->fc = fc; pre->sock = sock;
					
#ifdef USE_PTHREAD
						size_t stacksize = 16777216; //16 * 1024 * 1024;
						pthread_attr_t attr;
						pthread_attr_init( &attr );
						pthread_attr_setstacksize( &attr, stacksize );
						
						// Make sure we keep the number of threads under the limit
						if( pthread_create( &pre->thread, &attr, &FriendCoreProcess, ( void *)pre ) != 0 )
						{
							FFree( pre );
						}
#else
#ifdef USE_WORKERS
						SystemBase *locsb = (SystemBase *)fc->fci_SB;
						WorkerManagerRun( locsb->sl_WorkerManager,  FriendCoreProcess, pre, NULL );
						//WorkerManagerRun( fc->fci_WorkerManager,  FriendCoreProcess, pre );
#else
						int pid = fork();
						if( pid < 0 )
						{
							
						}else if( pid == 0 )
						{
							FriendCoreProcess( pre );
						}
						else
						{
							
						}
#endif
#endif
					}
				}
			}
		}
	}
	
	usleep( 1 );
	
	// check number of working threads
	while( TRUE )
	{
		if( nothreads <= 0 )
		{
			DEBUG("[FriendCoreEpoll] Number of threads %d\n", nothreads );
			break;
		}
		usleep( 5000 );
		DEBUG("[FriendCoreEpoll] Number of threads %d, waiting .....\n", nothreads );
	}
	
	// Free epoll events
	FFree( events );
	
	DEBUG( "[FriendCoreEpoll] Done freeing events" );
	events = NULL;
	
	// Free pipes!
	close( fc->fci_ReadCorePipe );
	close( fc->fci_WriteCorePipe );
	
	fc->fci_Closed = TRUE;
}
#endif

/**
 * Launches an instance of Friend Core.
 *
 * Only returns after interrupt signal
 *
 * @param fc pointer to Friend Core structure
 * @return 0 no errors
 * @return -1 in case of errors
 */
int FriendCoreRun( FriendCoreInstance* fc )
{
	// TODO:
	//     Launch ports and services from config file.
	//     Something like this, maybe:
	//     service{
	//         name     web
	//         host     somedomain.com
	//         protocol http
	//         port     6502
	//         handler  stdweb
	//     }
	//     service{
	//         name     web-load-balancer
	//         protocol http
	//         port     6503
	//         library  loadbalancer
	//     }
	//     service{
	//         name     friend-mesh
	//         protocol friendlish
	//         port     6503
	//     }

	LOG( FLOG_INFO,"=========================================\n");
	LOG( FLOG_INFO,"==========Starting FriendCore.===========\n");
	LOG( FLOG_INFO,"=========================================\n");
	
	SystemBase *lsb = (SystemBase *)fc->fci_SB;
	
	// Open new socket for lisenting

	fc->fci_Sockets = SocketOpen( lsb, fc->fci_SSLEnabled, fc->fci_Port, SOCKET_TYPE_SERVER );
	
	if( fc->fci_Sockets == NULL )
	{
		fc->fci_Closed = TRUE;
		return -1;
	}
	
	// Non blocking listening!
	if( SocketSetBlocking( fc->fci_Sockets, FALSE ) == -1 )
	{
		SocketClose( fc->fci_Sockets );
		fc->fci_Closed = TRUE;
		return -1;
	}
	
	if( SocketListen( fc->fci_Sockets ) != 0 )
	{
		SocketClose( fc->fci_Sockets );
		fc->fci_Closed= TRUE;
		return -1;
	}
	
	pipe2( fc->fci_SendPipe, 0 );
	pipe2( fc->fci_RecvPipe, 0 );

#ifdef USE_SELECT
	
	FriendCoreSelect( fc );
	
#else
	// Create epoll
	fc->fci_Epollfd = epoll_create1( 0 );
	if( fc->fci_Epollfd == -1 )
	{
		FERROR( "[FriendCore] epoll_create\n" );
		SocketClose( fc->fci_Sockets );
		fc->fci_Closed = TRUE;
		return -1;
	}

	// Register for events
	struct epoll_event event;
	memset( &event, 0, sizeof( event ) );
	event.data.fd = fc->fci_Sockets->fd;
	event.events = EPOLLIN | EPOLLET;
	
	if( epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_ADD, fc->fci_Sockets->fd, &event ) == -1 )
	{
		LOG( FLOG_ERROR, "[FriendCore] epoll_ctl\n" );
		SocketClose( fc->fci_Sockets );
		fc->fci_Closed = TRUE;
		return -1;
	}

	DEBUG("[FriendCore] Listening.\n");
	FriendCoreEpoll( fc );

	if( epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_DEL, fc->fci_Sockets->fd, &event ) == -1 )
	{
		LOG( FLOG_ERROR, "[FriendCore] epoll_ctl can not remove connection!\n" );
	}
#endif
	
	// Server is shutting down
	DEBUG("[FriendCore] Shutting down.\n");
	if( fc->fci_Sockets )
	{
		SocketClose( fc->fci_Sockets );
		fc->fci_Sockets = NULL;
		fc->fci_Closed = TRUE;
	}

	// Close libraries
	if( fc->fci_Libraries )
	{
		unsigned int iterator = 0;
		HashmapElement* e = NULL;
		while( ( e = HashmapIterate( fc->fci_Libraries, &iterator ) ) != NULL )
		{
			DEBUG( "[FriendCore] Closing library at address %lld\n", ( long long int )e->data );
			LibraryClose( (Library*)e->data );
			if( e->data != NULL )
			{
				FFree( e->data );
			}
			e->data = NULL;
			FFree( e->key );
			e->key = NULL;
		}
		HashmapFree( fc->fci_Libraries );
		fc->fci_Libraries = NULL;
	}
	
#ifdef USE_SELECT

#else
	// Close this
	if( fc->fci_Epollfd )
	{
		LOG( FLOG_INFO, "Closing Epoll file descriptor\n");
		close( fc->fci_Epollfd );
	}
#endif
	
	close( fc->fci_SendPipe[0] );
	close( fc->fci_SendPipe[1] );
	close( fc->fci_RecvPipe[0] );
	close( fc->fci_RecvPipe[1] );
	
	LOG( FLOG_INFO, "[FriendCore] Goodbye.\n");
	return 0;
}

/**
 * Opens a library into a Friend Code thread.
 *
 * @param fc pointer to Friend Core instance
 * @param libname name of the library to open
 * @param version version of the libray to open
 * @return NULL library could not be open
 */
Library* FriendCoreGetLibrary( FriendCoreInstance* fc, char* libname, FULONG version )
{
	if( !fc )
	{
		return NULL;
	}

	if( !fc->fci_Libraries )
	{
		fc->fci_Libraries = HashmapNew();
	}

	HashmapElement* e = HashmapGet( fc->fci_Libraries, libname );
	Library* lib = NULL;
	if( e == NULL )
	{
		SystemBase *lsb = (SystemBase *)fc->fci_SB;
		lib = LibraryOpen( lsb, libname, version );
		if( !lib )
		{
			return NULL;
		}
		HashmapPut( fc->fci_Libraries, StringDuplicate( libname ), lib );
	}
	else
	{
		lib = (Library*)e->data;
		if( lib->l_Version < version )
		{
			lib = NULL;
		}
	}
	return lib;
}

