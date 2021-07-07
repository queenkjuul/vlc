/*****************************************************************************
 * json.c: JSON tracer plugin
 *****************************************************************************
 * Copyright © 2021 Videolabs
 *
 * Authors : Nicolas Le Quec
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_fs.h>
#include <vlc_charset.h>
#include <vlc_tracer.h>
#include <vlc_memstream.h>

#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>

#define JSON_FILENAME "vlc-log.json"

typedef struct
{
    FILE *stream;
} vlc_tracer_sys_t;

static size_t UTF8ByteSequenceLength(unsigned char byte)
{
    if (byte <= 0x7F)
        return 1;
    if (0xC2 <= byte && byte <= 0xDF)
        return 2;
    else if (0xE0 <= byte && byte <= 0xEF)
        return 3;
    else if (0xF0 <= byte && byte <= 0xF4)
        return 4;
    else
        return 0;
}

static void JsonPrintString(FILE *stream, const char *str)
{
    if (!IsUTF8(str))
    {
        fputs("\"invalid string\"", stream);
        return;
    }

    fputc('\"', stream);

    unsigned char byte;
    while (*str != '\0')
    {
        switch (*str)
        {
        case '/':
            fputs("\\/", stream);
            break;
        case '\b':
            fputs("\\b", stream);
            break;
        case '\f':
            fputs("\\f", stream);
            break;
        case '\n':
            fputs("\\n", stream);
            break;
        case '\r':
            fputs("\\r", stream);
            break;
        case '\t':
            fputs("\\t", stream);
            break;
        case '\\':
        case '\"':
            fprintf(stream, "\\%c", *str);
            break;
        default:
            byte = *str;
            if (iscntrl(byte))
            {
                fprintf(stream, "\\u%02x", byte);
            }
            else
            {
                size_t byte_seq_len = UTF8ByteSequenceLength(byte);

                if (byte_seq_len > 0)
                {
                    fprintf(stream, "%.*s", (int) byte_seq_len, str);
                    str += byte_seq_len - 1;
                }
            }
        }
        str++;
    }
    fputc('\"', stream);
}

static void JsonPrintKeyValueNumber(FILE *stream, const char *key, int64_t value)
{
    JsonPrintString(stream, key);
    fprintf(stream, ": \"%"PRId64"\"", value);
}

static void JsonPrintKeyValueLabel(FILE *stream, const char *key, const char *value)
{
    JsonPrintString(stream, key);
    fputs(": ", stream);
    JsonPrintString(stream, value);
}

static void JsonStartObjectSection(FILE *stream, const char* name)
{
    if (name != NULL)
        fprintf(stream, "\"%s\": {", name);
    else
        fputc('{', stream);
}

static void JsonEndObjectSection(FILE *stream)
{
    fputc('}', stream);
}

static void TraceJson(void *opaque, va_list entries)
{
    vlc_tracer_sys_t *sys = opaque;
    FILE* stream = sys->stream;

    flockfile(stream);
    JsonStartObjectSection(stream, NULL);
    JsonPrintKeyValueNumber(stream, "Timestamp", US_FROM_VLC_TICK(vlc_tick_now()));
    fputc(',', stream);

    JsonStartObjectSection(stream, "Body");

    struct vlc_tracer_entry entry = va_arg(entries, struct vlc_tracer_entry);
    while (entry.key != NULL)
    {
        switch (entry.type)
        {
            case VLC_TRACER_INT:
                JsonPrintKeyValueNumber(stream, entry.key, entry.integer);
                break;
            case VLC_TRACER_STRING:
                JsonPrintKeyValueLabel(stream, entry.key, entry.string);
                break;
            default:
                vlc_assert_unreachable();
                break;
        }
        entry = va_arg(entries, struct vlc_tracer_entry);
        if (entry.key != NULL)
        {
            fputc(',', stream);
        }
    }
    JsonEndObjectSection(stream);
    JsonEndObjectSection(stream);
    fputc('\n', stream);
    funlockfile(stream);
}

static void Close(void *opaque)
{
    vlc_tracer_sys_t *sys = opaque;

    free(sys);
}

static const struct vlc_tracer_operations json_ops =
{
    TraceJson,
    Close
};

static const struct vlc_tracer_operations *Open(vlc_object_t *obj,
                                               void **restrict sysp)
{
    vlc_tracer_sys_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
        return NULL;

    const struct vlc_tracer_operations *ops = &json_ops;

    const char *filename = JSON_FILENAME;

    char *path = var_InheritString(obj, "tracer-file");
#ifdef __APPLE__
    if (path == NULL)
    {
        char *home = config_GetUserDir(VLC_HOME_DIR);
        if (home != NULL)
        {
            if (asprintf(&path, "%s/Library/Logs/"JSON_FILENAME, home) == -1)
                path = NULL;
            free(home);
        }
    }
#endif
    if (path != NULL)
        filename = path;

    /* Open the log file and remove any buffering for the stream */
    msg_Dbg(obj, "opening logfile `%s'", filename);
    sys->stream = vlc_fopen(filename, "at");
    if (sys->stream == NULL)
    {
        msg_Err(obj, "error opening log file `%s': %s", filename,
            vlc_strerror_c(errno) );
        free(path);
        free(sys);
        return NULL;
    }
    free(path);

    setvbuf(sys->stream, NULL, _IOLBF, 0);

    *sysp = sys;
    return ops;
}

#define FILE_LOG_TEXT N_("Log to file")
#define FILE_LOG_LONGTEXT N_("Log all VLC traces to a json file.")

#define LOGFILE_NAME_TEXT N_("Log filename")
#define LOGFILE_NAME_LONGTEXT N_("Specify the log filename.")

vlc_module_begin()
    set_shortname(N_("Tracer"))
    set_description(N_("JSON tracer"))
    set_category(CAT_ADVANCED)
    set_subcategory(SUBCAT_ADVANCED_MISC)
    set_capability("tracer", 0)
    set_callback(Open)

    add_savefile("tracer-file", NULL, LOGFILE_NAME_TEXT, LOGFILE_NAME_LONGTEXT)
vlc_module_end()
