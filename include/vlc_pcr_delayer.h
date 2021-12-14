/*****************************************************************************
 * vlc_pcr_delayer.h: Delay pcr events in asynchronous configurations
 *****************************************************************************
 * Copyright (C) 2021 VLC authors and VideoLAN
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

#ifndef VLC_PCR_DELAYER_H
#define VLC_PCR_DELAYER_H

#if defined(__cplusplus)
extern "C" {
#endif
// TODO cpp guard

/**
 * The PCR-Delayer utility API is to be used in modules that treats incomming data asynchronously
 * and are supposed to bind a PCR event to their output.
 */

/**
 * Opaque PCR-Delayer internal state.
 */
typedef struct vlc_pcr_delayer vlc_pcr_delayer_t;

/**
 * Allocate and initialize a new pcr_delayer.
 *
 * \param delay The media time delay in which the tool user is supposed to process data.
 *
 * \return An heap allocated pcr_delayer or NULL if the allocation failed.
 */
VLC_API vlc_pcr_delayer_t *vlc_pcr_delayer_New( vlc_tick_t delay ) VLC_USED;

/**
 * Release a pcr_delayer and discard its internal state.
 */
VLC_API void vlc_pcr_delayer_Release( vlc_pcr_delayer_t * );

/**
 * Signal a PCR event to the delayer.
 *
 * \note Passing the last received DTS is required to ease the delayer job when establishing a
 * PCR/DTS relationship.
 *
 * \param _ Self pointer.
 * \param pcr The PCR value.
 * \param block_dts Last received data dts from the caller module.
 *
 * \return TODO
 */
VLC_API int vlc_pcr_delayer_SignalPCR( vlc_pcr_delayer_t *, vlc_tick_t pcr, vlc_tick_t block_dts );

/**
 * Signal that a block entered the asynchronous delaying unit.
 *
 * \param _ Self pointer.
 * \param block The block (won't be treated as a block chain).
 *
 * \return TODO
 */
VLC_API int vlc_pcr_delayer_SignalEnteringBlock( vlc_pcr_delayer_t *, const block_t * );

/**
 * Signal that a block left the asynchronous delaying unit.
 *
 * \param _ Self pointer.
 * \param block The block (won't be treated as a block chain).
 *
 * \return TODO
 */
VLC_API int vlc_pcr_delayer_SignalLeavingBlock( vlc_pcr_delayer_t *, const block_t * );

/**
 * `vlc_pcr_delayer_BindPCRToDTS` output data.
 */
struct vlc_pcr_delayer_pair
{
    block_t *blocks; /**< blockchain start */
    vlc_tick_t pcr;  /**< PCR value if any. VLC_TICK_INVALID if none. */
};
/**
 * Extract blocks before the next recorded pcr event.
 *
 * \param _ Self pointer
 * \param out Block chain extract with pcr value bound to it.
 * \param blocks Block chain input
 *
 * \return The blockchain leftover. NULL if the chain was entirely consumed.
 */
VLC_API block_t *vlc_pcr_delayer_BindPCRToDTS( vlc_pcr_delayer_t *,
                                               struct vlc_pcr_delayer_pair *out,
                                               block_t *blocks );

#if defined(__cplusplus)
}
#endif

#endif
