/*****************************************************************************
 * hls.c: HLS Plugin descriptor
 *****************************************************************************
 * Copyright (C) 2022 VLC authors and VideoLAN
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>

#include <vlc_plugin.h>

#include "common.h"

#define SEGLEN_TEXT N_("Segment length")
#define SEGLEN_LONGTEXT N_("Length of TS stream segments")

#define SPLITANYWHERE_TEXT N_("Split segments anywhere")
#define SPLITANYWHERE_LONGTEXT                                                 \
    N_("Don't require a keyframe before splitting "                            \
       "a segment. Needed for audio only.")

#define NUMSEGS_TEXT N_("Number of segments")
#define NUMSEGS_LONGTEXT N_("Number of segments to include in index")

#define NOCACHE_TEXT N_("Allow cache")
#define NOCACHE_LONGTEXT                                                       \
    N_("Add EXT-X-ALLOW-CACHE:NO directive in playlist-file if this is "       \
       "disabled")

#define INDEX_TEXT N_("Index file")
#define INDEX_LONGTEXT N_("Path to the index file to create")

#define INDEXURL_TEXT N_("Full URL to put in index file")
#define INDEXURL_LONGTEXT                                                      \
    N_("Full URL to put in index file. "                                       \
       "Use #'s to represent segment number")

#define DELSEGS_TEXT N_("Delete segments")
#define DELSEGS_LONGTEXT N_("Delete segments when they are no longer needed")

#define RATECONTROL_TEXT N_("Use muxers rate control mechanism")

#define KEYURI_TEXT N_("AES key URI to place in playlist")

#define KEYFILE_TEXT N_("AES key file")
#define KEYFILE_LONGTEXT N_("File containing the 16 bytes encryption key")

#define KEYLOADFILE_TEXT N_("File where vlc reads key-uri and keyfile-location")
#define KEYLOADFILE_LONGTEXT                                                   \
    N_("File is read when segment starts and is assumed to be in format: "     \
       "key-uri\\nkey-file. File is read on the segment opening and "          \
       "values are used on that segment.")

#define RANDOMIV_TEXT N_("Use randomized IV for encryption")
#define RANDOMIV_LONGTEXT N_("Generate IV instead using segment-number as IV")

#define INTITIAL_SEG_TEXT N_("Number of first segment")
#define INITIAL_SEG_LONGTEXT N_("The number of the first segment generated")

vlc_module_begin()
    set_description(N_("HTTP Live streaming output"))
    set_shortname(N_("LiveHTTP"))
    add_shortcut("livehttp")
    set_capability("sout access", 0)
    set_subcategory(SUBCAT_SOUT_ACO)
    add_integer(ACCESS_CFG_PREFIX "seglen", 10, SEGLEN_TEXT, SEGLEN_LONGTEXT)
    add_integer(ACCESS_CFG_PREFIX "numsegs", 0, NUMSEGS_TEXT, NUMSEGS_LONGTEXT)
    add_integer(ACCESS_CFG_PREFIX "initial-segment-number", 1, INTITIAL_SEG_TEXT, INITIAL_SEG_LONGTEXT)
    add_bool(ACCESS_CFG_PREFIX "splitanywhere", false,
              SPLITANYWHERE_TEXT, SPLITANYWHERE_LONGTEXT)
    add_bool(ACCESS_CFG_PREFIX "delsegs", true,
              DELSEGS_TEXT, DELSEGS_LONGTEXT)
    add_bool(ACCESS_CFG_PREFIX "ratecontrol", false,
              RATECONTROL_TEXT, NULL)
    add_bool(ACCESS_CFG_PREFIX "caching", false,
              NOCACHE_TEXT, NOCACHE_LONGTEXT)
    add_bool(ACCESS_CFG_PREFIX "generate-iv", false,
              RANDOMIV_TEXT, RANDOMIV_LONGTEXT)
    add_string(ACCESS_CFG_PREFIX "index", NULL,
                INDEX_TEXT, INDEX_LONGTEXT)
    add_string(ACCESS_CFG_PREFIX "index-url", NULL,
                INDEXURL_TEXT, INDEXURL_LONGTEXT)
    add_string(ACCESS_CFG_PREFIX "key-uri", NULL,
                KEYURI_TEXT, NULL)
    add_loadfile(ACCESS_CFG_PREFIX "key-file", NULL,
                 KEYFILE_TEXT, KEYFILE_LONGTEXT)
    add_loadfile(ACCESS_CFG_PREFIX "key-loadfile", NULL,
                 KEYLOADFILE_TEXT, KEYLOADFILE_LONGTEXT)
    set_callbacks(AccessOpen, AccessClose)
vlc_module_end();
