/*****************************************************************************
 * emjsfile.c: emscripten js file access plugin
 *****************************************************************************
 * Copyright (C) 2022 VLC authors Videolabs, and VideoLAN
 *
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "vlc_block.h"
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_access.h>
#include <vlc_threads.h>
#include <stdalign.h>
#include <assert.h>

#include <emscripten.h>

enum state {
    IDLE,
    QUERIED,
    STOP,
};
struct worker_data {
    long id;
    uint64_t offset;
    uint64_t alignas(8) js_file_size;

    vlc_mutex_t lock;
    vlc_cond_t wait_state;
    vlc_cond_t wait_buffer;
    enum state state;
    block_t *buffer;

    vlc_sem_t rdy;
};

typedef struct
{
    struct worker_data data;
    vlc_thread_t worker;
} access_sys_t;


static block_t* Read (stream_t *p_access, bool* eof) {
    access_sys_t *p_sys = p_access->p_sys;

    const uint64_t offset = p_sys->data.offset;
    const uint64_t js_file_size = p_sys->data.js_file_size;

    if (offset >= js_file_size)
    {
        *eof = true;
        return NULL;
    }
    //if (size + offset > js_file_size)
    //    size = js_file_size - offset;

    vlc_mutex_lock(&p_sys->data.lock);
    p_sys->data.state = QUERIED;
    vlc_cond_signal(&p_sys->data.wait_state);

    while (p_sys->data.state == QUERIED)
        vlc_cond_wait(&p_sys->data.wait_buffer, &p_sys->data.lock);

    const enum state state = p_sys->data.state;
    block_t *block = p_sys->data.buffer;
    p_sys->data.buffer = NULL;
    vlc_mutex_unlock(&p_sys->data.lock);

    if (state == STOP)
    {
        *eof = true;
        return NULL;
    }
    return block;
}

static int Seek (stream_t *p_access, uint64_t offset) {
    access_sys_t *p_sys = p_access->p_sys;

    vlc_mutex_lock(&p_sys->data.lock);
    assert(offset < p_sys->data.js_file_size);
    p_sys->data.offset = offset;
    vlc_mutex_unlock(&p_sys->data.lock);
    return VLC_SUCCESS;
}

static int get_js_file_size(uint64_t *value) {
    /*
      Note :
      to avoid RangeError on BigUint64 view creation,
      the start offset (value) must be a multiple of 8.
    */
    assert(((uintptr_t)value % 8) == 0);
    return (EM_ASM_INT({
        try {
            var v = new BigUint64Array(wasmMemory.buffer, $0, 1);
            v[0] = BigInt(Module.vlcAccess.worker_js_file.size);
            return 0;
        }
        catch (error) {
            console.error("get_js_file_size error: " + error);
            return 1;
        }
    }, value) == 0) ? VLC_SUCCESS: VLC_EGENERIC;
}

static int Control( stream_t *p_access, int i_query, va_list args )
{
    bool    *pb_bool;
    vlc_tick_t *pi_64;
    access_sys_t *p_sys = p_access->p_sys;

    switch( i_query )
    {
        case STREAM_CAN_SEEK:
        case STREAM_CAN_FASTSEEK:
            pb_bool = va_arg( args, bool * );
            *pb_bool = true;
            break;

        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            pb_bool = va_arg( args, bool * );
            *pb_bool = true;
            break;

        case STREAM_GET_SIZE:
        {
            *va_arg( args, uint64_t * ) = p_sys->data.js_file_size;
            break;
        }

        case STREAM_GET_PTS_DELAY:
            pi_64 = va_arg( args, vlc_tick_t * );
            *pi_64 = VLC_TICK_FROM_MS(
                var_InheritInteger (p_access, "file-caching") );
            break;

        case STREAM_SET_PAUSE_STATE:
            break;

        default:
            return VLC_EGENERIC;

    }
    return VLC_SUCCESS;
}

EM_ASYNC_JS(int, init_js_file, (long id), {
    let p = new Promise((resolve, reject) => {
        function handleFileResult(e) {
            const msg = e['data'];
            if (msg.type === 'FileResult') {
                self.removeEventListener('message', handleFileResult);
                if (msg.file !== undefined) {
                    Module.vlcAccess.worker_js_file = msg.file;
                    Module.vlcAccess.reader = new FileReaderSync();
                    console.log("resolved");
                    resolve();
                }
                else {
                    reject("error: sent an undefined File object from the main thread");
                }
            }
            else if (msg.type === 'ErrorVLCAccessFileUndefined') {
                reject("error: vlc_access_file object is not defined");
            }
            else if (msg.type === 'ErrorRequestFileMessageWithoutId') {
                reject("error: request file message send without an id");
            }
            else if (msg.type === 'ErrorMissingFile') {
                reject("error: missing file, bad id or vlc_access_file[id] is not defined");
            }
        }
        self.addEventListener('message', handleFileResult);
    });
    let timer = undefined;
    let timeout = new Promise((resolve) => {
        timer = setTimeout(resolve, 1000, 'timeout');
    });
    let promises = [p, timeout];
    /* id must be unique */
    self.postMessage({ cmd: "customCmd", type: "requestFile", id: id});
    let return_value = 0;
    try {
        let value = await Promise.race(promises);
        if (value === 'timeout') {
            console.error("vlc_access timeout: could not get file!");
            return_value = 1;
        }
    }
    catch(error) {
        console.error("vlc_access error in init_js_file(): ", error);
        return_value = 1;
    }
    clearTimeout(timer);
    return return_value;
});

static void *WorkerLoop(void * opaque) {
    struct worker_data *data = opaque;

    EM_ASM({
        (Module.vlcAccess = {worker_js_file: undefined, reader: undefined});
    });

    /*
      This block will run in the main thread, to access the DOM.
      When the user selects a file, it is assigned to the Module.vlc_access_file
      array.
      
      We listen to 'message' events so that when the file is requested by the
      input thread, we can answer and send the File object from the main thread.
    */
    MAIN_THREAD_EM_ASM({
        const thread_id = $0;
        let w = Module.PThread.pthreads[thread_id];
        function handleFileRequest(e) {
            const msg = e.data;
            if (msg.type === "requestFile") {
                w.removeEventListener('message', handleFileRequest);
                if (Module.vlc_access_file === undefined) {
                    console.error("vlc_access_file property missing!");
                    w.postMessage({ cmd: "customCmd",
                            type: "ErrorVLCAccessFileUndefined"
                        });
                    return ;
                }
                if (msg.id === undefined) {
                    console.error("id property missing in requestFile message!");
                    w.postMessage({ cmd: "customCmd",
                            type: "ErrorRequestFileMessageWithoutId"
                        });
                    return ;
                }
                if (Module.vlc_access_file[msg.id] === undefined) {
                    console.error("error file missing!");
                    w.postMessage({ cmd: "customCmd",
                            type: "ErrorMissingFile"
                        });
                    return ;
                }
                /*
                  keeping the File object in the main thread too,
                  in case we want to reopen the media, without re-selecting
                  the file.
                */
                w.postMessage({ cmd: "customCmd",
                    type: "FileResult",
                    file: Module.vlc_access_file[msg.id]
                });
            }
        }
        w.addEventListener('message', handleFileRequest);
    }, pthread_self());


    /*
      Request the file from the main thread.
      If it was not selected, it will return an error.

      To open a file, we need to call libvlc_media_new_location with
      the following uri : emjsfile://<id>
    */
    if (init_js_file(data->id)) {
        return NULL;
    }

    get_js_file_size(&data->js_file_size);

    vlc_sem_post(&data->rdy);

    vlc_mutex_lock(&data->lock);
    for(;;) {
        while (data->state == IDLE)
            vlc_cond_wait(&data->wait_state, &data->lock);

        if (data->state == STOP)
            break;

        assert(data->offset < data->js_file_size);


#define BLOCK_SIZE (0x1000000u)
        uint64_t offset = data->offset;
        size_t size = BLOCK_SIZE;
        if (size + offset > data->js_file_size)
            size = data->js_file_size - offset;
        block_t *b = block_Alloc(size);

        EM_ASM({
            const offset = $0;
            const buffer = $1;
            const size = $2;
            const blob = Module.vlcAccess.worker_js_file.slice(offset, offset + size);
            HEAPU8.set(new Uint8Array(Module.vlcAccess.reader.readAsArrayBuffer(blob)), buffer);
        }, (double)offset, b->p_buffer, size);
        data->offset += size;
        if (data->buffer != NULL)
            block_Release(data->buffer);
        data->buffer = b;
        data->state = IDLE;
        vlc_cond_signal(&data->wait_buffer);
    }

    vlc_mutex_unlock(&data->lock);
    return NULL;
}

static void EmFileClose (vlc_object_t * p_this) {
    stream_t *p_access = (stream_t*)p_this;
    access_sys_t *sys= p_access->p_sys;
    printf("closing\n");

    vlc_mutex_lock(&sys->data.lock);
    sys->data.state = STOP;
    vlc_cond_signal(&sys->data.wait_state);
    vlc_cond_signal(&sys->data.wait_buffer);
    vlc_mutex_unlock(&sys->data.lock);

    //vlc_join(sys->worker, NULL);
}

static int EmFileOpen( vlc_object_t *p_this ) {
    stream_t *p_access = (stream_t*)p_this;

    printf("Open wtf\n");


    char *endPtr;
    long id = strtol(p_access->psz_location, &endPtr, 10);
    if ((endPtr == p_access->psz_location) || (*endPtr != '\0')) {
        msg_Err(p_access, "error: failed init uri has invalid id!");
        return VLC_EGENERIC;
    }

    access_sys_t *p_sys = vlc_obj_malloc(p_this, sizeof (*p_sys));
    if (unlikely(p_sys == NULL))
        return VLC_ENOMEM;

    p_sys->data.id = id;
    p_sys->data.offset = 0;
    p_sys->data.js_file_size = 0;
    vlc_mutex_init(&p_sys->data.lock);
    vlc_cond_init(&p_sys->data.wait_state);
    vlc_cond_init(&p_sys->data.wait_buffer);
    p_sys->data.state = IDLE;
    p_sys->data.buffer = NULL;
    vlc_sem_init(&p_sys->data.rdy, 0);

    if (vlc_clone(&p_sys->worker, WorkerLoop, &p_sys->data))
        return VLC_EGENERIC;
    vlc_sem_wait(&p_sys->data.rdy);

    //p_access->pf_read = Read;
    p_access->pf_block = Read;
    //p_access->pf_block = NULL;
    p_access->pf_control = Control;
    p_access->pf_seek = Seek;
    p_access->p_sys = p_sys;

    return VLC_SUCCESS;
}

vlc_module_begin ()
    set_description( N_("Emscripten module to allow reading local files from the DOM's <input>") )
    set_shortname( N_("Emscripten Local File Input") )
    set_subcategory( SUBCAT_INPUT_ACCESS )
    set_capability( "access", 0 )
    add_shortcut( "emjsfile" )
    set_callbacks( EmFileOpen, EmFileClose )
vlc_module_end()
