#include <assert.h>
#include <fcntl.h>

#include <vlc_block.h>
#include <vlc_fs.h>

#include "common.h"

typedef struct
{
    char *path;
    int fd;
    bool should_unlink;
} hls_storage_file_sys_t;

static void hls_storage_FileDestroy(hls_storage_t *self)
{
    hls_storage_file_sys_t *sys = self->sys;
    vlc_close(sys->fd);
    if (sys->should_unlink)
        vlc_unlink(sys->path);

    free(sys->path);
}

static block_t *hls_storage_FileWriteBlock(hls_storage_t *self, block_t *block)
{
    const hls_storage_file_sys_t *sys = self->sys;

    const ssize_t wrote = write(sys->fd, block->p_buffer, block->i_buffer);

    if (likely(wrote >= 0))
    {
        self->size += wrote;
    }

    const ssize_t bytes_left = block->i_buffer - wrote;
    block_t *ret = NULL;
    if (bytes_left > 0)
    {
        ret = block_Alloc(bytes_left);
        memcpy(ret->p_buffer, block->p_buffer + wrote, bytes_left);
    }

    block_Release(block);

    return ret;
}

static ssize_t hls_storage_FileWriteBytes(hls_storage_t *self,
                                          const uint8_t *bytes,
                                          size_t count)
{
    const hls_storage_file_sys_t *sys = self->sys;
    return write(sys->fd, bytes, count);
}

static int
hls_storage_FileRead(const hls_storage_t *self, uint8_t buffer[], size_t size)
{
    const hls_storage_file_sys_t *sys = self->sys;

    assert(size >= self->size);

    const int read_fd = vlc_open(sys->path, O_RDONLY);
    if (unlikely(read_fd == -1))
        return VLC_EGENERIC;

    const size_t to_read = __MIN(self->size, size);
    size_t data_read = 0u;
    while (data_read < to_read)
    {
        const ssize_t currently_read = read(read_fd, buffer, to_read);
        if (unlikely(currently_read == -1))
            return VLC_EGENERIC;
        data_read += currently_read;
    }

    vlc_close(read_fd);

    return VLC_SUCCESS;
}

hls_storage_t *
hls_storage_FromFile(const char *path, int flags, bool unlink_on_destroy)
{
    const int fd = vlc_open(path, flags, 0666);
    if (fd == -1)
        return NULL;

    hls_storage_t *ret = malloc(sizeof(*ret));
    if (unlikely(ret == NULL))
        goto err;

    hls_storage_file_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        goto err;
    sys->fd = fd;
    sys->should_unlink = unlink_on_destroy;
    sys->path = strdup(path);
    if (unlikely(sys->path == NULL))
    {
        free(sys);
        return NULL;
    }

    ret->sys = sys;
    ret->size = 0u;
    ret->write_block = hls_storage_FileWriteBlock;
    ret->write_bytes = hls_storage_FileWriteBytes;
    ret->read = hls_storage_FileRead;
    ret->destroy = hls_storage_FileDestroy;

    return ret;

err:
    free(ret);
    vlc_close(fd);
    return NULL;
}

typedef struct
{
    block_t *begin;
    block_t **end;
    vlc_mutex_t lock;
} hls_storage_memory_sys_t;

static void hls_storage_MemoryDestroy(hls_storage_t *self)
{
    hls_storage_memory_sys_t *sys = self->sys;
    vlc_mutex_lock(&sys->lock);
    block_ChainRelease(sys->begin);
    vlc_mutex_unlock(&sys->lock);
}

static block_t *hls_storage_MemoryWriteBlock(hls_storage_t *self,
                                             block_t *block)
{
    assert(block != NULL);
    hls_storage_memory_sys_t *sys = self->sys;

    vlc_mutex_lock(&sys->lock);
    block_ChainLastAppend(&sys->end, block);
    self->size += block->i_buffer;
    vlc_mutex_unlock(&sys->lock);

    return NULL;
}

static ssize_t hls_storage_MemoryWriteBytes(hls_storage_t *self,
                                            const uint8_t *buff,
                                            size_t count)
{
    hls_storage_memory_sys_t *sys = self->sys;

    uint8_t *dup = malloc(count);
    if (unlikely(dup == NULL))
        return -1;

    memcpy(dup, buff, count);

    block_t *b = block_heap_Alloc(dup, count);

    vlc_mutex_lock(&sys->lock);
    block_ChainLastAppend(&sys->end, b);
    self->size += count;
    vlc_mutex_unlock(&sys->lock);
}

static int
hls_storage_MemoryRead(const hls_storage_t *self, uint8_t buffer[], size_t size)
{
    hls_storage_memory_sys_t *sys = self->sys;
    assert(size >= self->size);

    const size_t to_read = __MIN(self->size, size);
    size_t total = 0u;
    const block_t *it = sys->begin;
    while (total < to_read)
    {
        if (it->i_buffer > size - total)
            return VLC_EGENERIC;

        memcpy(buffer, it->p_buffer, it->i_buffer);
        buffer += it->i_buffer;
        total += it->i_buffer;
    }
    return VLC_SUCCESS;
}

hls_storage_t *hls_storage_MemoryNew(void)
{
    hls_storage_t *storage = malloc(sizeof(*storage));
    if (unlikely(storage == NULL))
        return NULL;

    hls_storage_memory_sys_t *sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
    {
        free(storage);
        return NULL;
    }

    sys->begin = NULL;
    sys->end = &sys->begin;
    vlc_mutex_init(&sys->lock);

    storage->sys = sys;
    storage->size = 0u;
    storage->write_block = hls_storage_MemoryWriteBlock;
    storage->write_bytes = hls_storage_MemoryWriteBytes;
    storage->read = hls_storage_MemoryRead;
    storage->destroy = hls_storage_MemoryDestroy;

    return storage;
}

void hls_storage_Destroy(hls_storage_t *storage)
{
    storage->destroy(storage);
    free(storage->sys);
    free(storage);
}
