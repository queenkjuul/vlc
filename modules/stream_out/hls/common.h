#ifndef HLS_COMMON_H
#define HLS_COMMON_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_httpd.h>

#define SOUT_CFG_PREFIX "sout-hls-"
int SoutOpen( vlc_object_t * );
void SoutClose( vlc_object_t * );

#define ACO_CFG_PREFIX "aco-hls-"
int AccessOpen( vlc_object_t * );
void AccessClose( vlc_object_t * );

/*****************************************************************************
 * IO
 *****************************************************************************/

typedef struct hls_io hls_io;

/*
 * Create a new io descriptor.
 * if path is NULL flags is ignored and a memory-only io is returned.
 */
hls_io *hls_io_New( const char *path, int flags );

typedef struct
{
    /* Usual IO manipulations */
    int ( *open )( hls_io * );
    /* Write a block using the IO implementation, this function takes ownership
     * of the block.
     * Returns data not consumed (if any) in a new block */
    block_t *( *consume_block )( hls_io *, block_t * );
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

typedef struct
{
    char *psz_filename;
    char *psz_path;
    char *psz_uri;
    char *psz_key_uri;
    char *psz_duration;
    vlc_tick_t segment_length;
    uint32_t i_segment_number;
    uint8_t aes_ivs[16];
    hls_io *io_handle;
    // httpd_url_t *httpd_file;
    bool closed;
} hls_segment;

typedef struct {
    const hls_io *handle;
    vlc_mutex_t lock;
} hls_index;

#define HLS_SOUT_CALLBACKS_VAR "sout-hls-callbacks"
struct hls_sout_callbacks
{
    void *sys;

    int ( *segment_added )( void *,
                            const sout_access_out_t *,
                            const hls_segment * );
    void ( *segment_removed )( void *,
                               const sout_access_out_t *,
                               const hls_segment * );

    void ( *index_updated )( void *,
                             const sout_access_out_t *,
                             const hls_io * );
};

int url_segment_cb( const hls_segment *segment,
                    httpd_client_t *cl,
                    httpd_message_t *answer,
                    const httpd_message_t *query );
int url_index_cb( hls_index *index,
                  httpd_client_t *cl,
                  httpd_message_t *answer,
                  const httpd_message_t *query );
#endif
