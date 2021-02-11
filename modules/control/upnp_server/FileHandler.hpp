#pragma once

#include "upnp_server.h"

struct FileHandler
{
    virtual bool get_info( UpnpFileInfo& info ) noexcept = 0;
    virtual bool open(intf_thread_t *) noexcept = 0;
    virtual size_t read( uint8_t[], size_t ) noexcept = 0;

    enum class SeekType : int
    {
        Set = SEEK_SET,
        Current = SEEK_CUR,
        End = SEEK_END
    };
    virtual bool seek( SeekType, off_t ) noexcept = 0;

    virtual ~FileHandler() = default;
};

std::unique_ptr<FileHandler> parse_url(const char *url, const intf_sys_t&) noexcept;
