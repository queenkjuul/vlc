/*****************************************************************************
 * livehttp.c: Live HTTP Streaming
 *****************************************************************************
 * Copyright © 2001, 2002, 2013 VLC authors and VideoLAN
 * Copyright © 2009-2010 by Keary Griffin
 *
 * Authors: Keary Griffin <kearygriffin at gmail.com>
 *          Ilkka Ollakka <ileoo at videolan dot org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <vlc_common.h>

#include <vlc_block.h>
#include <vlc_charset.h>
#include <vlc_fs.h>
#include <vlc_httpd.h>
#include <vlc_memstream.h>
#include <vlc_plugin.h>
#include <vlc_sout.h>
#include <vlc_strings.h>

#include <gcrypt.h>
#include <vlc_gcrypt.h>

#include <vlc_rand.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#define STR_ENDLIST "#EXT-X-ENDLIST\n"

#define MAX_RENAME_RETRIES 10

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int Open( vlc_object_t * );
static void Close( vlc_object_t * );

#define SOUT_CFG_PREFIX "sout-livehttp-"
#define SEGLEN_TEXT N_( "Segment length" )
#define SEGLEN_LONGTEXT N_( "Length of TS stream segments" )
#define SPLITANYWHERE_TEXT N_( "Split segments anywhere" )
#define SPLITANYWHERE_LONGTEXT                                                 \
    N_( "Don't require a keyframe before splitting "                           \
        "a segment. Needed for audio only." )

#define NUMSEGS_TEXT N_( "Number of segments" )
#define NUMSEGS_LONGTEXT N_( "Number of segments to include in index" )

#define NOCACHE_TEXT N_( "Allow cache" )
#define NOCACHE_LONGTEXT                                                       \
    N_( "Add EXT-X-ALLOW-CACHE:NO directive in playlist-file if this is "      \
        "disabled" )

#define INDEX_TEXT N_( "Index file" )
#define INDEX_LONGTEXT N_( "Path to the index file to create" )

#define INDEXURL_TEXT N_( "Full URL to put in index file" )
#define INDEXURL_LONGTEXT                                                      \
    N_( "Full URL to put in index file. "                                      \
        "Use #'s to represent segment number" )

#define DELSEGS_TEXT N_( "Delete segments" )
#define DELSEGS_LONGTEXT N_( "Delete segments when they are no longer needed" )

#define RATECONTROL_TEXT N_( "Use muxers rate control mechanism" )

#define KEYURI_TEXT N_( "AES key URI to place in playlist" )

#define KEYFILE_TEXT N_( "AES key file" )
#define KEYFILE_LONGTEXT N_( "File containing the 16 bytes encryption key" )

#define KEYLOADFILE_TEXT                                                       \
    N_( "File where vlc reads key-uri and keyfile-location" )
#define KEYLOADFILE_LONGTEXT                                                   \
    N_( "File is read when segment starts and is assumed to be in format: "    \
        "key-uri\\nkey-file. File is read on the segment opening and "         \
        "values are used on that segment." )

#define RANDOMIV_TEXT N_( "Use randomized IV for encryption" )
#define RANDOMIV_LONGTEXT N_( "Generate IV instead using segment-number as IV" )

#define INTITIAL_SEG_TEXT N_( "Number of first segment" )
#define INITIAL_SEG_LONGTEXT N_( "The number of the first segment generated" )

#define NO_FS_TEXT                                                             \
    N_( "Don't store the segments on the filesystem (live only)" )
#define NO_FS_LONGTEXT                                                         \
    N_( "The segmented track will be stored in memory instead of on the "      \
        "filesystem, be cautious as it can lead to big memory consumption if " \
        "the segment number is too high" )

/* clang-format off */
vlc_module_begin ()
    set_description( N_("HTTP Live streaming output") )
    set_shortname( N_("LiveHTTP" ))
    add_shortcut( "livehttp" )
    set_capability( "sout access", 0 )
    set_category( CAT_SOUT )
    set_subcategory( SUBCAT_SOUT_ACO )
    add_integer( SOUT_CFG_PREFIX "seglen", 10, SEGLEN_TEXT, SEGLEN_LONGTEXT )
    add_integer( SOUT_CFG_PREFIX "numsegs", 0, NUMSEGS_TEXT, NUMSEGS_LONGTEXT )
    add_integer( SOUT_CFG_PREFIX "initial-segment-number", 1, INTITIAL_SEG_TEXT, INITIAL_SEG_LONGTEXT )
    add_bool( SOUT_CFG_PREFIX "splitanywhere", false,
              SPLITANYWHERE_TEXT, SPLITANYWHERE_LONGTEXT )
    add_bool( SOUT_CFG_PREFIX "delsegs", true,
              DELSEGS_TEXT, DELSEGS_LONGTEXT )
    add_bool( SOUT_CFG_PREFIX "ratecontrol", false,
              RATECONTROL_TEXT, RATECONTROL_TEXT )
    add_bool( SOUT_CFG_PREFIX "caching", false,
              NOCACHE_TEXT, NOCACHE_LONGTEXT )
    add_bool( SOUT_CFG_PREFIX "generate-iv", false,
              RANDOMIV_TEXT, RANDOMIV_LONGTEXT )
    add_bool( SOUT_CFG_PREFIX "no-fs", false,
              NO_FS_TEXT, NO_FS_LONGTEXT )
    add_string( SOUT_CFG_PREFIX "index", NULL,
                INDEX_TEXT, INDEX_LONGTEXT )
    add_string( SOUT_CFG_PREFIX "index-url", NULL,
                INDEXURL_TEXT, INDEXURL_LONGTEXT )
    add_string( SOUT_CFG_PREFIX "key-uri", NULL,
                KEYURI_TEXT, NULL )
    add_loadfile(SOUT_CFG_PREFIX "key-file", NULL,
                 KEYFILE_TEXT, KEYFILE_LONGTEXT)
    add_loadfile(SOUT_CFG_PREFIX "key-loadfile", NULL,
                 KEYLOADFILE_TEXT, KEYLOADFILE_LONGTEXT)
    set_callbacks( Open, Close )
vlc_module_end ();
/* clang-format on */

/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/
static const char *const ppsz_sout_options[] = { "seglen",
                                                 "splitanywhere",
                                                 "numsegs",
                                                 "delsegs",
                                                 "index",
                                                 "index-url",
                                                 "ratecontrol",
                                                 "caching",
                                                 "key-uri",
                                                 "key-file",
                                                 "key-loadfile",
                                                 "generate-iv",
                                                 "initial-segment-number",
                                                 "no-fs",
                                                 NULL };

static ssize_t Write( sout_access_out_t *, block_t * );
static int Control( sout_access_out_t *, int, va_list );

/*****************************************************************************
 * IO
 *****************************************************************************/

typedef struct hls_io hls_io;

static hls_io *hls_io_NewFile( const char *path, int flags );
static hls_io *hls_io_NewBlockChain();

typedef struct
{
    /* Usual IO manipulations */
    int ( *open )( hls_io * );
    /* Write a block using the IO implementation, this function takes ownership
     * of the block */
    block_t *( *consume )( hls_io *, block_t * );
    void ( *close )( hls_io * );

    /* Reading is an opaque value providing the necessary tools for
     * multithreaded reads */
    void *( *new_reading_context )( const hls_io * );
    void ( *release_reading_context )( void * );
    ssize_t ( *read )( void *, uint8_t[], size_t );

    /* Destroy and/or unlink the IO representation, used to get rid of
     * segments content before actually releasing the data structure */
    int ( *unlink )( hls_io * );

    /* Release IO internal state */
    void ( *release )( hls_io * );
} hls_io_ops;

struct hls_io
{
    void *sys;
    size_t size;

    hls_io_ops ops;
};

typedef struct output_segment
{
    char *psz_filename;
    char *psz_uri;
    char *psz_key_uri;
    char *psz_duration;
    vlc_tick_t segment_length;
    uint32_t i_segment_number;
    uint8_t aes_ivs[16];
    hls_io *io_handle;
    httpd_url_t *httpd_file;
    bool closed;
} output_segment_t;

typedef struct
{
    char *psz_cursegPath;
    char *psz_indexPath;
    char *psz_indexUrl;
    char *psz_keyfile;
    vlc_tick_t last_segment_exposed_time;
    vlc_tick_t segment_max_length;
    vlc_tick_t current_segment_length;
    uint32_t i_segment;
    block_t *full_segments;
    block_t **full_segments_end;
    block_t *ongoing_segment;
    block_t **ongoing_segment_end;
    unsigned i_numsegs;
    unsigned i_initial_segment;
    bool b_delsegs;
    bool b_ratecontrol;
    bool b_splitanywhere;
    bool b_caching;
    bool b_generate_iv;
    bool b_segment_has_data;
    bool b_no_fs;
    uint8_t aes_ivs[16];
    gcry_cipher_hd_t aes_ctx;
    char *key_uri;
    block_t *stuffing_bytes;
    ssize_t stuffing_size;
    vlc_array_t segments_t;

    hls_io *p_playlist;
    vlc_mutex_t playlist_lock;
    httpd_url_t *playlist_file;

    httpd_host_t *http_host;
} sout_access_out_sys_t;

static int url_playlist_cb( httpd_callback_sys_t *data,
                            httpd_client_t *cl,
                            httpd_message_t *answer,
                            const httpd_message_t *query )
{
    if ( !answer || !query || !cl )
        return VLC_SUCCESS;

    sout_access_out_sys_t *p_sys = (sout_access_out_sys_t *)data;

    const hls_io *io = p_sys->p_playlist;

    vlc_mutex_lock( &p_sys->playlist_lock );

    if ( io == NULL )
        goto err;

    void *reading_context = io->ops.new_reading_context( io );
    if ( reading_context == NULL )
        goto err;

    answer->i_body = io->size;
    answer->p_body = malloc( io->size );
    if ( unlikely( data == NULL ) )
        goto err;

    size_t to_read = io->size;
    size_t cursor = 0;
    while ( to_read != 0 )
    {
        const ssize_t read =
            io->ops.read( reading_context, answer->p_body + cursor, to_read );

        if ( read == -1 )
        {
            io->ops.release_reading_context( reading_context );
            goto err;
        }

        cursor += read;
        to_read -= read;
    }

    io->ops.release_reading_context( reading_context );

    vlc_mutex_unlock( &p_sys->playlist_lock );

    answer->i_proto = HTTPD_PROTO_HTTP;
    answer->i_version = 0;
    answer->i_type = HTTPD_MSG_ANSWER;
    answer->i_status = 200;

    httpd_MsgAdd( answer, "Content-type", "application/vnd.apple.mpegurl" );
    httpd_MsgAdd( answer, "Cache-Control", "no-cache" );
    httpd_MsgAdd( answer, "Connection", "close" );
    httpd_MsgAdd( answer, "Access-Control-Allow-Origin", "*" );
    httpd_MsgAdd( answer, "Access-Control-Expose-Headers", "Content-Length" );
    return VLC_SUCCESS;

err:
    vlc_mutex_unlock( &p_sys->playlist_lock );
    free( answer->p_body );
    return VLC_EGENERIC;
}

static int url_segment_cb( httpd_callback_sys_t *data,
                           httpd_client_t *cl,
                           httpd_message_t *answer,
                           const httpd_message_t *query )
{
    if ( !answer || !query || !cl )
        return VLC_SUCCESS;

    const output_segment_t *segment = (output_segment_t *)data;
    const hls_io *io = segment->io_handle;

    printf( "fetching %s\n", segment->psz_filename );
    ;

    void *reading_context = io->ops.new_reading_context( io );
    if ( reading_context == NULL )
        return VLC_EGENERIC;

    assert( io->size );
    assert( segment->closed );

    answer->p_body = malloc( io->size );

    size_t to_read = io->size;
    size_t cursor = 0;
    while ( to_read != 0 )
    {
        const ssize_t read =
            io->ops.read( reading_context, answer->p_body + cursor, to_read );

        if ( read == -1 )
            goto err;

        cursor += read;
        to_read -= read;
    }

    io->ops.release_reading_context( reading_context );
    answer->i_body = cursor;

    answer->i_proto = HTTPD_PROTO_HTTP;
    answer->i_version = 0;
    answer->i_type = HTTPD_MSG_ANSWER;
    answer->i_status = 200;

    httpd_MsgAdd( answer, "Content-type", "video/mpeg" );
    httpd_MsgAdd( answer, "Cache-Control", "no-cache" );
    httpd_MsgAdd( answer, "Connection", "close" );
    httpd_MsgAdd( answer, "Access-Control-Allow-Origin", "*" );
    httpd_MsgAdd( answer, "Access-Control-Expose-Headers", "Content-Length" );
    return VLC_SUCCESS;
err:
    io->ops.release_reading_context( reading_context );
    free( answer->p_body );
    return VLC_EGENERIC;
}

static int LoadCryptFile( sout_access_out_t *p_access );
static int CryptSetup( sout_access_out_t *p_access, char *keyfile );
static int CheckSegmentChange( sout_access_out_t *p_access, block_t *p_buffer );
static ssize_t writeSegment( hls_io *handle, sout_access_out_t *p_access );
static ssize_t openNextFile( sout_access_out_t *p_access,
                             sout_access_out_sys_t *p_sys );
/*****************************************************************************
 * Open: open the file
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_access_out_t *p_access = (sout_access_out_t *)p_this;
    sout_access_out_sys_t *p_sys;
    char *psz_idx;

    config_ChainParse( p_access, SOUT_CFG_PREFIX, ppsz_sout_options,
                       p_access->p_cfg );

    if ( !p_access->psz_path )
    {
        msg_Err( p_access, "no file name specified" );
        return VLC_EGENERIC;
    }

    if ( unlikely( !( p_sys = calloc( 1, sizeof( *p_sys ) ) ) ) )
        return VLC_ENOMEM;

    /* Try to get within asked segment length */
    size_t i_seglen = var_GetInteger( p_access, SOUT_CFG_PREFIX "seglen" );

    p_sys->segment_max_length = vlc_tick_from_sec( i_seglen );
    p_sys->full_segments = NULL;
    p_sys->full_segments_end = &p_sys->full_segments;

    p_sys->ongoing_segment = NULL;
    p_sys->ongoing_segment_end = &p_sys->ongoing_segment;

    p_sys->i_numsegs = var_GetInteger( p_access, SOUT_CFG_PREFIX "numsegs" );
    p_sys->i_initial_segment =
        var_GetInteger( p_access, SOUT_CFG_PREFIX "initial-segment-number" );
    p_sys->b_splitanywhere =
        var_GetBool( p_access, SOUT_CFG_PREFIX "splitanywhere" );
    p_sys->b_delsegs = var_GetBool( p_access, SOUT_CFG_PREFIX "delsegs" );
    p_sys->b_ratecontrol =
        var_GetBool( p_access, SOUT_CFG_PREFIX "ratecontrol" );
    p_sys->b_caching = var_GetBool( p_access, SOUT_CFG_PREFIX "caching" );
    p_sys->b_generate_iv =
        var_GetBool( p_access, SOUT_CFG_PREFIX "generate-iv" );
    p_sys->b_segment_has_data = false;

    vlc_array_init( &p_sys->segments_t );

    p_sys->stuffing_bytes = block_Alloc( 16 );
    p_sys->stuffing_size = 0;

    p_sys->psz_indexPath = NULL;
    psz_idx = var_GetNonEmptyString( p_access, SOUT_CFG_PREFIX "index" );
    if ( psz_idx )
    {
        char *psz_tmp;
        psz_tmp = vlc_strftime( psz_idx );
        free( psz_idx );
        if ( !psz_tmp )
        {
            free( p_sys );
            return VLC_ENOMEM;
        }
        p_sys->psz_indexPath = psz_tmp;
        if ( p_sys->i_initial_segment != 1 )
            vlc_unlink( p_sys->psz_indexPath );
    }

    p_sys->psz_indexUrl =
        var_GetNonEmptyString( p_access, SOUT_CFG_PREFIX "index-url" );
    p_sys->psz_keyfile =
        var_GetNonEmptyString( p_access, SOUT_CFG_PREFIX "key-loadfile" );
    p_sys->key_uri =
        var_GetNonEmptyString( p_access, SOUT_CFG_PREFIX "key-uri" );

    p_access->p_sys = p_sys;

    if ( p_sys->psz_keyfile && ( LoadCryptFile( p_access ) < 0 ) )
    {
        free( p_sys->psz_indexUrl );
        free( p_sys->psz_indexPath );
        free( p_sys );
        msg_Err( p_access, "Encryption init failed" );
        return VLC_EGENERIC;
    }
    else if ( !p_sys->psz_keyfile && ( CryptSetup( p_access, NULL ) < 0 ) )
    {
        free( p_sys->psz_indexUrl );
        free( p_sys->psz_indexPath );
        free( p_sys );
        msg_Err( p_access, "Encryption init failed" );
        return VLC_EGENERIC;
    }

    p_sys->i_segment = p_sys->i_initial_segment - 1;
    p_sys->psz_cursegPath = NULL;

    p_access->pf_write = Write;
    p_access->pf_control = Control;

    p_sys->http_host = vlc_http_HostNew( VLC_OBJECT( p_access ) );
    assert( p_sys->http_host ); // TODO actual check

    vlc_mutex_init( &p_sys->playlist_lock );

    p_sys->playlist_file =
        httpd_UrlNew( p_sys->http_host, "/mystream", NULL, NULL );
    httpd_UrlCatch( p_sys->playlist_file, HTTPD_MSG_GET, url_playlist_cb,
                    (void *)p_sys );
    p_sys->last_segment_exposed_time = VLC_TICK_INVALID;

    return VLC_SUCCESS;
}

/************************************************************************
 * CryptSetup: Initialize encryption
 ************************************************************************/
static int CryptSetup( sout_access_out_t *p_access, char *key_file )
{
    sout_access_out_sys_t *p_sys = p_access->p_sys;
    uint8_t key[16];
    char *keyfile = NULL;

    if ( !p_sys->key_uri ) /*No key uri, assume no encryption wanted*/
    {
        msg_Dbg( p_access, "No key uri, no encryption" );
        return VLC_SUCCESS;
    }

    if ( key_file )
        keyfile = strdup( key_file );
    else
        keyfile = var_InheritString( p_access, SOUT_CFG_PREFIX "key-file" );

    if ( unlikely( keyfile == NULL ) )
    {
        msg_Err( p_access, "No key-file, no encryption" );
        return VLC_EGENERIC;
    }

    vlc_gcrypt_init();

    /*Setup encryption cipher*/
    gcry_error_t err = gcry_cipher_open( &p_sys->aes_ctx, GCRY_CIPHER_AES,
                                         GCRY_CIPHER_MODE_CBC, 0 );
    if ( err )
    {
        msg_Err( p_access, "Openin AES Cipher failed: %s",
                 gpg_strerror( err ) );
        free( keyfile );
        return VLC_EGENERIC;
    }

    int keyfd = vlc_open( keyfile, O_RDONLY | O_NONBLOCK );
    if ( unlikely( keyfd == -1 ) )
    {
        msg_Err( p_access, "Unable to open keyfile %s: %s", keyfile,
                 vlc_strerror_c( errno ) );
        free( keyfile );
        gcry_cipher_close( p_sys->aes_ctx );
        return VLC_EGENERIC;
    }
    free( keyfile );

    ssize_t keylen = read( keyfd, key, 16 );

    vlc_close( keyfd );
    if ( keylen < 16 )
    {
        msg_Err( p_access,
                 "No key at least 16 octects (you provided %zd), no encryption",
                 keylen );
        gcry_cipher_close( p_sys->aes_ctx );
        return VLC_EGENERIC;
    }

    err = gcry_cipher_setkey( p_sys->aes_ctx, key, 16 );
    if ( err )
    {
        msg_Err( p_access, "Setting AES key failed: %s", gpg_strerror( err ) );
        gcry_cipher_close( p_sys->aes_ctx );
        return VLC_EGENERIC;
    }

    if ( p_sys->b_generate_iv )
        vlc_rand_bytes( p_sys->aes_ivs, sizeof( uint8_t ) * 16 );

    return VLC_SUCCESS;
}

/************************************************************************
 * LoadCryptFile: Try to parse key_uri and keyfile-location from file
 ************************************************************************/
static int LoadCryptFile( sout_access_out_t *p_access )
{
    sout_access_out_sys_t *p_sys = p_access->p_sys;

    FILE *stream = vlc_fopen( p_sys->psz_keyfile, "rt" );
    char *key_file = NULL, *key_uri = NULL;

    if ( unlikely( stream == NULL ) )
    {
        msg_Err( p_access, "Unable to open keyloadfile %s: %s",
                 p_sys->psz_keyfile, vlc_strerror_c( errno ) );
        return VLC_EGENERIC;
    }

    // First read key_uri
    ssize_t len = getline( &key_uri, &( size_t ){ 0 }, stream );
    if ( unlikely( len == -1 ) )
    {
        msg_Err( p_access, "Cannot read %s: %s", p_sys->psz_keyfile,
                 vlc_strerror_c( errno ) );
        clearerr( stream );
        fclose( stream );
        free( key_uri );
        return VLC_EGENERIC;
    }
    // Strip the newline from uri, maybe scanf would be better?
    key_uri[len - 1] = '\0';

    len = getline( &key_file, &( size_t ){ 0 }, stream );
    if ( unlikely( len == -1 ) )
    {
        msg_Err( p_access, "Cannot read %s: %s", p_sys->psz_keyfile,
                 vlc_strerror_c( errno ) );
        clearerr( stream );
        fclose( stream );

        free( key_uri );
        free( key_file );
        return VLC_EGENERIC;
    }
    // Strip the last newline from filename
    key_file[len - 1] = '\0';
    fclose( stream );

    int returncode = VLC_SUCCESS;
    if ( !p_sys->key_uri || strcmp( p_sys->key_uri, key_uri ) )
    {
        if ( p_sys->key_uri )
        {
            free( p_sys->key_uri );
            p_sys->key_uri = NULL;
        }
        p_sys->key_uri = strdup( key_uri );
        returncode = CryptSetup( p_access, key_file );
    }
    free( key_file );
    free( key_uri );
    return returncode;
}

/************************************************************************
 * CryptKey: Set encryption IV to current segment number
 ************************************************************************/
static int CryptKey( sout_access_out_t *p_access, uint32_t i_segment )
{
    sout_access_out_sys_t *p_sys = p_access->p_sys;

    if ( !p_sys->b_generate_iv )
    {
        /* Use segment number as IV if randomIV isn't selected*/
        memset( p_sys->aes_ivs, 0, 16 * sizeof( uint8_t ) );
        p_sys->aes_ivs[15] = i_segment & 0xff;
        p_sys->aes_ivs[14] = ( i_segment >> 8 ) & 0xff;
        p_sys->aes_ivs[13] = ( i_segment >> 16 ) & 0xff;
        p_sys->aes_ivs[12] = ( i_segment >> 24 ) & 0xff;
    }

    gcry_error_t err = gcry_cipher_setiv( p_sys->aes_ctx, p_sys->aes_ivs, 16 );
    if ( err )
    {
        msg_Err( p_access, "Setting AES IVs failed: %s", gpg_strerror( err ) );
        gcry_cipher_close( p_sys->aes_ctx );
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

#define SEG_NUMBER_PLACEHOLDER "#"
/*****************************************************************************
 * formatSegmentPath: create segment path name based on seg #
 *****************************************************************************/
static char *formatSegmentPath( char *psz_path, uint32_t i_seg )
{
    char *psz_result;
    char *psz_firstNumSign;

    if ( !( psz_result = vlc_strftime( psz_path ) ) )
        return NULL;

    psz_firstNumSign =
        psz_result + strcspn( psz_result, SEG_NUMBER_PLACEHOLDER );
    if ( *psz_firstNumSign )
    {
        char *psz_newResult;
        int i_cnt = strspn( psz_firstNumSign, SEG_NUMBER_PLACEHOLDER );
        int ret;

        *psz_firstNumSign = '\0';
        ret = asprintf( &psz_newResult, "%s%0*d%s", psz_result, i_cnt, i_seg,
                        psz_firstNumSign + i_cnt );
        free( psz_result );
        if ( ret < 0 )
            return NULL;
        psz_result = psz_newResult;
    }

    return psz_result;
}

static void destroySegment( output_segment_t *segment )
{
    hls_io *handler = segment->io_handle;
    // handler->ops.destroy( handler );
    handler->ops.release( handler );
    free( handler );
    free( segment->psz_filename );
    free( segment->psz_duration );
    free( segment->psz_uri );
    free( segment->psz_key_uri );
    httpd_UrlDelete( segment->httpd_file );
    free( segment );
}

/************************************************************************
 * segmentAmountNeeded: check that playlist has atleast
 *3*p_sys->segment_max_length of segments return how many segments are needed
 *for that (max of p_sys->i_segment )
 ************************************************************************/
static uint32_t segmentAmountNeeded( sout_access_out_sys_t *p_sys )
{
    vlc_tick_t duration = 0;
    for ( size_t index = 1; index <= vlc_array_count( &p_sys->segments_t );
          index++ )
    {
        output_segment_t *segment = vlc_array_item_at_index(
            &p_sys->segments_t, vlc_array_count( &p_sys->segments_t ) - index );
        duration += segment->segment_length;

        if ( duration >= ( 3 * p_sys->segment_max_length ) )
            return __MAX( index, p_sys->i_numsegs );
    }
    return vlc_array_count( &p_sys->segments_t ) - 1;
}

/************************************************************************
 * isFirstItemRemovable: Check for draft 11 section 6.2.2
 * check that the first item has been around outside playlist
 * segment->segment_/24length + (p_sys->i_numsegs * p_sys->segment_max_length)
 *before it is removed.
 ************************************************************************/
static bool isFirstItemRemovable( sout_access_out_sys_t *p_sys,
                                  uint32_t i_firstseg,
                                  uint32_t i_index_offset )
{
    vlc_tick_t duration = 0;

    /* Check that segment has been out of playlist for segment_length +
     * (p_sys->i_numsegs * p_sys->segment_max_length) amount We check this by
     * calculating duration of the items that replaced first item in playlist
     */
    for ( unsigned int index = 0; index < i_index_offset; index++ )
    {
        output_segment_t *segment = vlc_array_item_at_index(
            &p_sys->segments_t, p_sys->i_segment - i_firstseg + index );
        duration += segment->segment_length;
    }
    output_segment_t *first = vlc_array_item_at_index( &p_sys->segments_t, 0 );

    return duration >= ( first->segment_length +
                         ( p_sys->i_numsegs * p_sys->segment_max_length ) );
}

/************************************************************************
 * updateIndexAndDel: If necessary, update index file & delete old segments
 ************************************************************************/
static int updateIndexAndDel( sout_access_out_t *p_access,
                              sout_access_out_sys_t *p_sys,
                              bool b_isend )
{

    uint32_t i_firstseg;
    unsigned i_index_offset = 0;

    if ( p_sys->i_numsegs == 0 ||
         p_sys->i_segment < ( p_sys->i_numsegs + p_sys->i_initial_segment ) )
    {
        i_firstseg = p_sys->i_initial_segment;
    }
    else
    {
        unsigned numsegs = segmentAmountNeeded( p_sys );
        i_firstseg = ( p_sys->i_segment - numsegs ) + 1;
        i_index_offset = vlc_array_count( &p_sys->segments_t ) - numsegs;
    }

    // First update index
    if ( p_sys->psz_indexPath )
    {
        int val;
        struct vlc_memstream ms;

        // FILE *fp;
        // char *psz_idxTmp;
        // if ( asprintf( &psz_idxTmp, "%s.tmp", p_sys->psz_indexPath ) < 0 )
        //    return -1;

        // fp = vlc_fopen( psz_idxTmp, "wt" );
        if ( vlc_memstream_open( &ms ) != VLC_SUCCESS )
        {
            // msg_Err( p_access, "cannot open index file `%s'", psz_idxTmp );
            // free( psz_idxTmp );
            return -1;
        }

        int status = vlc_memstream_printf(
            &ms,
            "#EXTM3U\n#EXT-X-TARGETDURATION:%.0f\n#EXT-X-VERSION:3\n#"
            "EXT-X-ALLOW-CACHE:%s"
            "%s\n#EXT-X-MEDIA-SEQUENCE:%" PRIu32 "\n%s",
            ceil( secf_from_vlc_tick( p_sys->segment_max_length ) ),
            p_sys->b_caching ? "YES" : "NO",
            p_sys->i_numsegs > 0 ? ""
            : b_isend            ? "\n#EXT-X-PLAYLIST-TYPE:VOD"
                                 : "\n#EXT-X-PLAYLIST-TYPE:EVENT",
            i_firstseg,
            ( ( p_sys->i_initial_segment > 1 ) &&
              ( p_sys->i_initial_segment == i_firstseg ) )
                ? "#EXT-X-DISCONTINUITY\n"
                : "" );
        if ( status < 0 )
        {
            //            free( psz_idxTmp );
            //           fclose( fp );
            return -1;
        }

        char *psz_current_uri = NULL;

        for ( uint32_t i = i_firstseg; i <= p_sys->i_segment; i++ )
        {
            // scale to i_index_offset..numsegs + i_index_offset
            uint32_t index = i - i_firstseg + i_index_offset;

            output_segment_t *segment =
                vlc_array_item_at_index( &p_sys->segments_t, index );
            if ( p_sys->key_uri &&
                 ( !psz_current_uri ||
                   strcmp( psz_current_uri, segment->psz_key_uri ) ) )
            {
                int ret = 0;
                free( psz_current_uri );
                psz_current_uri = strdup( segment->psz_key_uri );
                if ( p_sys->b_generate_iv )
                {
                    unsigned long long iv_hi = segment->aes_ivs[0];
                    unsigned long long iv_lo = segment->aes_ivs[8];
                    for ( unsigned short j = 1; j < 8; j++ )
                    {
                        iv_hi <<= 8;
                        iv_hi |= segment->aes_ivs[j] & 0xff;
                        iv_lo <<= 8;
                        iv_lo |= segment->aes_ivs[8 + j] & 0xff;
                    }
                    ret = vlc_memstream_printf(
                        &ms,
                        "#EXT-X-KEY:METHOD=AES-128,URI=\"%s\",IV=0X%"
                        "16.16llx%16.16llx\n",
                        segment->psz_key_uri, iv_hi, iv_lo );
                }
                else
                {
                    ret = vlc_memstream_printf(
                        &ms, "#EXT-X-KEY:METHOD=AES-128,URI=\"%s\"\n",
                        segment->psz_key_uri );
                }
                if ( ret < 0 )
                {
                    free( psz_current_uri );
                    // free( psz_idxTmp );
                    // fclose( fp );
                    return -1;
                }
            }

            val =
                vlc_memstream_printf( &ms, "#EXTINF:%s,\n%s\n",
                                      segment->psz_duration, segment->psz_uri );
            if ( val < 0 )
            {
                free( psz_current_uri );
                // free( psz_idxTmp );
                // fclose( fp );
                return -1;
            }
        }
        free( psz_current_uri );

        if ( b_isend )
        {
            if ( vlc_memstream_write( &ms, STR_ENDLIST,
                                      sizeof( STR_ENDLIST ) - 1 ) < 0 )
            {
                //                free( psz_idxTmp );
                //                fclose( fp );
                return -1;
            }
        }
        // fclose( fp );

        status = vlc_memstream_close( &ms );
        assert( status == VLC_SUCCESS );

        block_t *clone = block_Alloc( ms.length );
        memcpy( clone->p_buffer, ms.ptr, ms.length );
        free( ms.ptr );

        vlc_mutex_lock( &p_sys->playlist_lock );

        if ( p_sys->p_playlist != NULL )
        {
            p_sys->p_playlist->ops.unlink( p_sys->p_playlist );
            p_sys->p_playlist->ops.release( p_sys->p_playlist );
            free( p_sys->p_playlist );
        }

        // p_sys->p_playlist = hls_io_NewBlockChain();
        p_sys->p_playlist = hls_io_NewFile( p_sys->psz_indexPath,
                                            O_WRONLY | O_CREAT | O_TRUNC );
        if ( unlikely( p_sys->p_playlist == NULL ) )
        {
            // TODO proper error handling
            vlc_mutex_unlock( &p_sys->playlist_lock );
            return -1;
        }

        p_sys->p_playlist->ops.open( p_sys->p_playlist );
        p_sys->p_playlist->ops.consume( p_sys->p_playlist, clone );
        p_sys->p_playlist->ops.close( p_sys->p_playlist );

        vlc_mutex_unlock( &p_sys->playlist_lock );

        // val = vlc_rename( psz_idxTmp, p_sys->psz_indexPath );

        // if ( val < 0 )
        // {
        //     vlc_unlink( psz_idxTmp );
        //     msg_Err( p_access, "Error moving LiveHttp index file" );
        // }
        // else
        //     msg_Dbg( p_access, "LiveHttpIndexComplete: %s",
        //              p_sys->psz_indexPath );

        // free( psz_idxTmp );
    }

    // Then take care of deletion
    // Try to follow pantos draft 11 section 6.2.2
    while ( p_sys->b_delsegs && p_sys->i_numsegs &&
            isFirstItemRemovable( p_sys, i_firstseg, i_index_offset ) )
    {
        output_segment_t *segment =
            vlc_array_item_at_index( &p_sys->segments_t, 0 );
        msg_Dbg( p_access, "Removing segment number %d",
                 segment->i_segment_number );
        vlc_array_remove( &p_sys->segments_t, 0 );

        destroySegment( segment );
        i_index_offset -= 1;
    }

    return 0;
}

/*****************************************************************************
 * closeCurrentSegment: Close the segment file
 *****************************************************************************/
static void closeCurrentSegment( sout_access_out_t *p_access,
                                 sout_access_out_sys_t *p_sys,
                                 bool b_isend )
{
    size_t seg_count = vlc_array_count( &p_sys->segments_t );

    hls_io *handle = seg_count ? ( (output_segment_t *)vlc_array_item_at_index(
                                       &p_sys->segments_t, seg_count - 1 ) )
                                     ->io_handle
                               : NULL;
    if ( handle != NULL )
    {
        output_segment_t *segment = (output_segment_t *)vlc_array_item_at_index(
            &p_sys->segments_t, seg_count - 1 );
        hls_io *handle = seg_count ? segment->io_handle : NULL;

        if ( p_sys->key_uri )
        {
            block_t *stuffing = p_sys->stuffing_bytes;
            const size_t pad = stuffing->i_buffer - p_sys->stuffing_size;
            memset( &stuffing->p_buffer[p_sys->stuffing_size], pad, pad );
            gcry_error_t err =
                gcry_cipher_encrypt( p_sys->aes_ctx, stuffing->p_buffer,
                                     stuffing->i_buffer, NULL, 0 );

            if ( err )
            {
                msg_Err( p_access, "Couldn't encrypt 16 bytes: %s",
                         gpg_strerror( err ) );
            }
            else
            {
                block_t *ret = handle->ops.consume(
                    handle, block_Duplicate( p_sys->stuffing_bytes ) );
                if ( ret != NULL )
                {
                    block_Release( ret );
                    msg_Err( p_access, "Couldn't write 16 bytes" );
                }
            }
            p_sys->stuffing_size = 0;
        }

        handle->ops.close( handle );
        segment->closed = true;

        if ( !( us_asprintf(
                 &segment->psz_duration, "%.2f",
                 secf_from_vlc_tick( p_sys->current_segment_length ) ) ) )
        {
            msg_Err( p_access, "Couldn't set duration on closed segment" );
            return;
        }
        segment->segment_length = p_sys->current_segment_length;

        segment->i_segment_number = p_sys->i_segment;

        if ( p_sys->psz_cursegPath )
        {
            msg_Dbg( p_access, "LiveHttpSegmentComplete: %s (%" PRIu32 ")",
                     p_sys->psz_cursegPath, p_sys->i_segment );
            free( p_sys->psz_cursegPath );
            p_sys->psz_cursegPath = 0;
            updateIndexAndDel( p_access, p_sys, b_isend );
        }
    }
}

/*****************************************************************************
 * Close: close the target
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    sout_access_out_t *p_access = (sout_access_out_t *)p_this;
    sout_access_out_sys_t *p_sys = p_access->p_sys;

    if ( p_sys->ongoing_segment )
        block_ChainLastAppend( &p_sys->full_segments_end,
                               p_sys->ongoing_segment );
    p_sys->ongoing_segment = NULL;
    p_sys->ongoing_segment_end = &p_sys->ongoing_segment;

    block_t *output_block = p_sys->full_segments;
    p_sys->full_segments = NULL;
    p_sys->full_segments_end = &p_sys->full_segments;

    while ( output_block )
    {
        block_t *p_next = output_block->p_next;
        output_block->p_next = NULL;

        Write( p_access, output_block );
        output_block = p_next;
    }
    if ( p_sys->ongoing_segment )
    {
        block_ChainLastAppend( &p_sys->full_segments_end,
                               p_sys->ongoing_segment );
        p_sys->ongoing_segment = NULL;
        p_sys->ongoing_segment_end = &p_sys->ongoing_segment;
    }

    size_t seg_count = vlc_array_count( &p_sys->segments_t );
    output_segment_t *segment =
        seg_count ? (output_segment_t *)vlc_array_item_at_index(
                        &p_sys->segments_t, seg_count - 1 )

                  : NULL;
    hls_io *handle = segment ? segment->io_handle : NULL;

    ssize_t writevalue = writeSegment( handle, p_access );
    msg_Dbg( p_access, "Writing.. %zd", writevalue );
    if ( unlikely( writevalue < 0 ) )
    {
        if ( p_sys->full_segments )
            block_ChainRelease( p_sys->full_segments );
        if ( p_sys->ongoing_segment )
            block_ChainRelease( p_sys->ongoing_segment );
    }

    closeCurrentSegment( p_access, p_sys, true );
    vlc_tick_sleep( p_sys->segment_max_length + VLC_TICK_FROM_SEC( 1 ) );

    httpd_UrlDelete( p_sys->playlist_file );

    vlc_mutex_lock( &p_sys->playlist_lock );
    if ( p_sys->p_playlist )
    {
        p_sys->p_playlist->ops.unlink( p_sys->p_playlist );
        p_sys->p_playlist->ops.release( p_sys->p_playlist );
        free( p_sys->p_playlist );
    }
    vlc_mutex_unlock( &p_sys->playlist_lock );

    // Wait a sufficient amount of time (according to the standard) before
    // removing the segments.
    // TODO: This should be implemented only when draining as it blocks the
    // Close for a big amount of time.
    const vlc_tick_t duration =
        segment->segment_length +
        ( p_sys->i_numsegs * p_sys->segment_max_length );
    vlc_tick_sleep( duration );

    while ( vlc_array_count( &p_sys->segments_t ) > 0 )
    {
        output_segment_t *segment =
            vlc_array_item_at_index( &p_sys->segments_t, 0 );
        vlc_array_remove( &p_sys->segments_t, 0 );
        if ( p_sys->b_delsegs && p_sys->i_numsegs && segment->psz_filename )
        {
            msg_Dbg( p_access, "Removing segment number %d name %s",
                     segment->i_segment_number, segment->psz_filename );
            hls_io *handle = segment->io_handle;
            handle->ops.unlink( handle );
        }

        destroySegment( segment );
    }

    if ( p_sys->key_uri )
    {
        gcry_cipher_close( p_sys->aes_ctx );
        free( p_sys->key_uri );
    }

    block_Release( p_sys->stuffing_bytes );

    httpd_HostDelete( p_sys->http_host );

    free( p_sys->psz_indexUrl );
    free( p_sys->psz_indexPath );
    free( p_sys );

    msg_Dbg( p_access, "livehttp access output closed" );
}

static int Control( sout_access_out_t *p_access, int i_query, va_list args )
{
    sout_access_out_sys_t *p_sys = p_access->p_sys;

    switch ( i_query )
    {
    case ACCESS_OUT_CONTROLS_PACE:
    {
        bool *pb = va_arg( args, bool * );
        *pb = !p_sys->b_ratecontrol;
        //*pb = true;
        break;
    }

    default:
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * openNextFile: Open the segment file
 *****************************************************************************/
static ssize_t openNextFile( sout_access_out_t *p_access,
                             sout_access_out_sys_t *p_sys )
{
    uint32_t i_newseg = p_sys->i_segment + 1;

    /* Create segment and fill it info that we can (everything excluding
     * duration */
    output_segment_t *segment =
        (output_segment_t *)calloc( 1, sizeof( output_segment_t ) );
    if ( unlikely( !segment ) )
        return -1;

    segment->i_segment_number = i_newseg;
    segment->psz_filename = formatSegmentPath( p_access->psz_path, i_newseg );
    char *psz_idxFormat =
        p_sys->psz_indexUrl ? p_sys->psz_indexUrl : p_access->psz_path;
    segment->psz_uri = formatSegmentPath( psz_idxFormat, i_newseg );

    if ( unlikely( !segment->psz_filename ) )
    {
        msg_Err( p_access, "Format segmentpath failed" );
        destroySegment( segment );
        return -1;
    }

    // segment->p_handle = hls_io_NewFile(
    //     segment->psz_filename, O_WRONLY | O_CREAT | O_LARGEFILE | O_TRUNC );
    segment->io_handle = hls_io_NewBlockChain();
    if ( segment->io_handle == NULL ||
         segment->io_handle->ops.open( segment->io_handle ) != VLC_SUCCESS )
    {
        msg_Err( p_access, "cannot open `%s' (%s)", segment->psz_filename,
                 vlc_strerror_c( errno ) );
        destroySegment( segment );
        return -1;
    }

    vlc_array_append_or_abort( &p_sys->segments_t, segment );

    if ( p_sys->psz_keyfile )
    {
        LoadCryptFile( p_access );
    }

    if ( p_sys->key_uri )
    {
        segment->psz_key_uri = strdup( p_sys->key_uri );
        CryptKey( p_access, i_newseg );
        if ( p_sys->b_generate_iv )
            memcpy( segment->aes_ivs, p_sys->aes_ivs, sizeof( uint8_t ) * 16 );
    }
    printf( "%s\n", segment->psz_filename );
    segment->httpd_file =
        httpd_UrlNew( p_sys->http_host, segment->psz_filename, NULL, NULL );
    assert( segment->httpd_file );
    httpd_UrlCatch( segment->httpd_file, HTTPD_MSG_GET, &url_segment_cb,
                    (void *)segment );
    msg_Dbg( p_access, "Successfully opened livehttp file: %s (%" PRIu32 ")",
             segment->psz_uri, i_newseg );

    p_sys->psz_cursegPath = strdup( segment->psz_filename );
    p_sys->i_segment = i_newseg;
    p_sys->b_segment_has_data = false;
    return 1;
}
/*****************************************************************************
 * CheckSegmentChange: Check if segment needs to be closed and new opened
 *****************************************************************************/
static int CheckSegmentChange( sout_access_out_t *p_access, block_t *p_buffer )
{
    sout_access_out_sys_t *p_sys = p_access->p_sys;
    ssize_t writevalue = 0;

    vlc_tick_t current_length = 0;
    vlc_tick_t ongoing_length = 0;

    size_t seg_count = vlc_array_count( &p_sys->segments_t );
    const output_segment_t *segment =
        seg_count ? (output_segment_t *)vlc_array_item_at_index(
                        &p_sys->segments_t, seg_count - 1 )
                  : NULL;

    block_ChainProperties( p_sys->full_segments, NULL, NULL, &current_length );
    block_ChainProperties( p_sys->ongoing_segment, NULL, NULL,
                           &ongoing_length );

    if ( segment != NULL &&
         ( ( p_buffer->i_length + current_length + ongoing_length ) >=
           p_sys->segment_max_length ) )
    {

        hls_io *handle = seg_count ? segment->io_handle : NULL;
        writevalue = writeSegment( handle, p_access );
        if ( unlikely( writevalue < 0 ) )
        {
            block_ChainRelease( p_buffer );
            return -1;
        }
        if ( p_sys->last_segment_exposed_time != VLC_TICK_INVALID )
        {
            const vlc_tick_t now = vlc_tick_now();
            const vlc_tick_t sleep = p_sys->current_segment_length -
                                     ( now - p_sys->last_segment_exposed_time );
            printf( "SLEEPING %d, current length=%d\n",
                    MS_FROM_VLC_TICK( sleep ),
                    MS_FROM_VLC_TICK( p_sys->current_segment_length ) );
            vlc_tick_sleep( sleep );
        }
        closeCurrentSegment( p_access, p_sys, false );
        p_sys->last_segment_exposed_time = vlc_tick_now();
    }

    // const size_t num_seg = var_GetInteger(p_access, "numsegs");
    // if ( num_seg == 0 || seg_count < num_seg )
    if ( segment == NULL || segment->closed )
    {
        if ( openNextFile( p_access, p_sys ) < 0 )
            return -1;
    }
    return writevalue;
}

static gcry_error_t encryptBlock( sout_access_out_sys_t *p_sys,
                                  block_t *output )
{
    if ( p_sys->stuffing_size )
    {
        output =
            block_Realloc( output, p_sys->stuffing_size, output->i_buffer );
        if ( unlikely( !output ) )
            return VLC_ENOMEM;
        memcpy( output->p_buffer, p_sys->stuffing_bytes, p_sys->stuffing_size );
        p_sys->stuffing_size = 0;
    }
    size_t original = output->i_buffer;
    size_t padded = ( output->i_buffer + 15 ) & ~15;
    size_t pad = padded - original;
    if ( pad )
    {
        p_sys->stuffing_size = 16 - pad;
        output->i_buffer -= p_sys->stuffing_size;
        memcpy( p_sys->stuffing_bytes, &output->p_buffer[output->i_buffer],
                p_sys->stuffing_size );
    }

    return gcry_cipher_encrypt( p_sys->aes_ctx, output->p_buffer,
                                output->i_buffer, NULL, 0 );
}

static ssize_t writeSegment( hls_io *handle, sout_access_out_t *p_access )
{
    sout_access_out_sys_t *p_sys = p_access->p_sys;
    msg_Dbg( p_access, "Writing all full segments" );

    block_t *output = p_sys->full_segments;
    p_sys->full_segments = NULL;
    p_sys->full_segments_end = &p_sys->full_segments;

    vlc_tick_t current_length = 0;
    block_ChainProperties( output, NULL, NULL, &current_length );

    ssize_t i_write = 0;
    bool crypted = false;
    p_sys->current_segment_length = current_length;
    while ( output )
    {
        if ( p_sys->key_uri && !crypted )
        {
            const gcry_error_t err = encryptBlock( p_sys, output );
            if ( err )
            {
                msg_Err( p_access, "Encryption failure: %s ",
                         gpg_strerror( err ) );
                return -1;
            }
            crypted = true;
        }

        block_t *p_next = output->p_next;

        output->p_next = NULL;

        const size_t output_size = output->i_buffer;

        block_t *not_wrote = handle->ops.consume( handle, output );

        // if ( val == -1 )
        //{
        //    if ( errno == EINTR )
        //        continue;
        //    return -1;
        //}

        if ( not_wrote == NULL )
        {
            output = p_next;
            crypted = false;
            i_write += output_size;
        }
        else
        {
            not_wrote->p_next = p_next;
            output = not_wrote;
            i_write += output_size - not_wrote->i_buffer;
        }
    }
    return i_write;
}

/*****************************************************************************
 * Write: standard write on a file descriptor.
 *****************************************************************************/
static ssize_t Write( sout_access_out_t *p_access, block_t *p_buffer )
{
    size_t i_write = 0;
    sout_access_out_sys_t *p_sys = p_access->p_sys;

    //    const size_t count = vlc_array_count( &p_sys->segments_t );
    //   if (count == p_sys->i_numsegs)
    //   {
    //       const output_segment_t *first = vlc_array_item_at_index(
    //       &p_sys->segments_t, 0 ); vlc_tick_t now = vlc_tick_now();
    //       printf("SLEEPING %d\n", first->exposed_time - now +
    //       first->segment_length); vlc_tick_sleep(first->exposed_time - now +
    //       first->segment_length);
    //   }

    while ( p_buffer )
    {
        /* Check if current block is already past segment-length
            and we want to write gathered blocks into segment
            and update playlist */
        if ( p_sys->ongoing_segment &&
             ( p_sys->b_splitanywhere ||
               ( p_buffer->i_flags & BLOCK_FLAG_HEADER ) ) )
        {
            msg_Dbg( p_access,
                     "Moving ongoing segment to full segments-queue" );
            block_ChainLastAppend( &p_sys->full_segments_end,
                                   p_sys->ongoing_segment );
            p_sys->ongoing_segment = NULL;
            p_sys->ongoing_segment_end = &p_sys->ongoing_segment;
            p_sys->b_segment_has_data = true;
        }

        ssize_t ret = CheckSegmentChange( p_access, p_buffer );
        if ( ret < 0 )
        {
            msg_Err( p_access, "Error in write loop" );
            return ret;
        }
        i_write += ret;

        block_t *p_temp = p_buffer->p_next;
        p_buffer->p_next = NULL;
        block_ChainLastAppend( &p_sys->ongoing_segment_end, p_buffer );
        p_buffer = p_temp;
    }

    return i_write;
}

/*****************************************************************************
 * IO: File
 *****************************************************************************/
typedef struct
{
    char *path;
    int flags;
    int fd;
} hls_io_FileData;

static int file_open( hls_io *self )
{
    hls_io_FileData *sys = self->sys;
    assert( sys->path != NULL );
    assert( sys->fd == -1 );

    sys->fd = vlc_open( sys->path, sys->flags, 0666 );
    return sys->fd == -1 ? VLC_EGENERIC : VLC_SUCCESS;
}

static void file_close( hls_io *self )
{
    hls_io_FileData *sys = self->sys;
    assert( sys->fd != -1 );

    vlc_close( sys->fd );
    sys->fd = -1;
}

static block_t *file_consume( hls_io *self, block_t *block )
{
    const hls_io_FileData *sys = self->sys;
    assert( sys->fd != -1 );

    const ssize_t wrote = write( sys->fd, block->p_buffer, block->i_buffer );

    if ( likely( wrote >= 0 ) )
        self->size += wrote;

    const ssize_t bytes_left = block->i_buffer - wrote;
    block_t *ret = NULL;
    if ( bytes_left > 0 )
    {
        ret = block_Alloc( bytes_left );
        memcpy( ret->p_buffer, block->p_buffer + wrote, bytes_left );
    }

    block_Release( block );

    return ret;
}

struct hls_io_FileRContext
{
    int fd;
};

static void *file_new_reading_context( const hls_io *self )
{
    const hls_io_FileData *sys = self->sys;

    struct hls_io_FileRContext *ret = calloc( 1, sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    ret->fd = open( sys->path, O_RDONLY | O_LARGEFILE );
    if ( unlikely( ret->fd == -1 ) )
    {
        free( ret );
        return NULL;
    }

    return ret;
}

static void file_release_reading_context( void *opaque )
{
    struct hls_io_FileRContext *context = opaque;

    assert( context->fd > 0 );
    close( context->fd );

    free( context );
}

static ssize_t file_read( void *reading_context, uint8_t buffer[], size_t size )
{
    const struct hls_io_FileRContext *context = reading_context;
    assert( context->fd != -1 );
    return read( context->fd, buffer, size );
}

static int file_unlink( hls_io *self )
{
    const hls_io_FileData *sys = self->sys;
    assert( sys->path != NULL );

    return vlc_unlink( sys->path );
}

static void file_release( hls_io *self )
{
    hls_io_FileData *sys = self->sys;
    if ( sys->fd != -1 )
        vlc_close( sys->fd );
    free( sys->path );
    free( sys );
}

static hls_io *hls_io_NewFile( const char *path, int flags )
{
    hls_io *ret = calloc( 1, sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    hls_io_FileData *sys = malloc( sizeof( *sys ) );
    if ( unlikely( sys == NULL ) )
    {
        goto err;
    }

    *sys =
        ( hls_io_FileData ){ .path = strdup( path ), .flags = flags, .fd = -1 };
    if ( unlikely( sys->path == NULL ) )
    {
        goto err;
    }
    ret->sys = sys;

    ret->ops =
        ( hls_io_ops ){ .open = file_open,
                        .consume = file_consume,
                        .new_reading_context = file_new_reading_context,
                        .release_reading_context = file_release_reading_context,
                        .read = file_read,
                        .close = file_close,
                        .unlink = file_unlink,
                        .release = file_release };
    return ret;

err:
    free( ret );
    return NULL;
}

/*****************************************************************************
 * IO: BlockChain
 *****************************************************************************/

typedef struct
{
    block_t *begin;
    block_t *end;
} hls_io_BlockChainData;

static int blockchain_open( hls_io *self )
{
    printf( "OPEN\n" );
    const hls_io_BlockChainData *sys = self->sys;
    assert( sys->begin == NULL );
    assert( sys->end == NULL );
    return VLC_SUCCESS;
}

static void blockchain_close( hls_io *self ) { (void)self; }

static block_t *blockchain_consume( hls_io *self, block_t *block )
{
    assert( block != NULL );
    hls_io_BlockChainData *sys = self->sys;

    block_ChainAppend( &sys->end, block );

    sys->end = block;

    if ( sys->begin == NULL )
        sys->begin = block;

    self->size += block->i_buffer;

    return NULL;
}

struct hls_io_BlockChainRContext
{
    const block_t *current_block;

    /* If read stop in the middle of a block buffer, this cursor keeps track of
     * the byte where copy stopped. */
    size_t block_cursor;
};

static void *blockchain_new_reading_context( const hls_io *self )
{
    printf( "RCONTEXT\n" );
    const hls_io_BlockChainData *sys = self->sys;

    struct hls_io_BlockChainRContext *ret = malloc( sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    *ret = ( struct hls_io_BlockChainRContext ){ .current_block = sys->begin,
                                                 .block_cursor = 0 };

    return ret;
}

static ssize_t
blockchain_read( void *reading_context, uint8_t buffer[], size_t size )
{
    printf( "READ\n" );
    struct hls_io_BlockChainRContext *context = reading_context;

    size_t total = 0;
    while ( context->current_block != NULL && size != 0 )
    {
        const block_t *current = context->current_block;

        const size_t copy =
            __MIN( size, current->i_buffer - context->block_cursor );

        memcpy( buffer, current->p_buffer + context->block_cursor, copy );

        if ( copy == size )
        {
            /* We need to stop reading in the middle of a block. We then
             * remember the block buffer copy position */
            context->block_cursor = copy;
        }
        else
        {
            context->block_cursor = 0;
            context->current_block = current->p_next;
        }

        size -= copy;
        buffer += copy;
        total += copy;
    }
    return total;
}

static int blockchain_unlink( hls_io *self )
{
    return VLC_SUCCESS;
    (void)self;
}

static void blockchain_release( hls_io *self )
{
    printf( "RELEASE\n" );
    hls_io_BlockChainData *sys = self->sys;
    if ( sys->begin != NULL )
        block_ChainRelease( sys->begin );

    free( self->sys );
}

static hls_io *hls_io_NewBlockChain()
{
    printf( "NEW\n" );
    hls_io *ret = calloc( 1, sizeof( *ret ) );
    if ( unlikely( ret == NULL ) )
        return NULL;

    ret->sys = calloc( 1, sizeof( hls_io_BlockChainData ) );
    if ( unlikely( ret->sys == NULL ) )
    {
        free( ret );
        return NULL;
    }

    ret->ops =
        ( hls_io_ops ){ .open = blockchain_open,
                        .consume = blockchain_consume,
                        .new_reading_context = blockchain_new_reading_context,
                        .release_reading_context = free,
                        .read = blockchain_read,
                        .close = blockchain_close,
                        .unlink = blockchain_unlink,
                        .release = blockchain_release };
    return ret;
}
