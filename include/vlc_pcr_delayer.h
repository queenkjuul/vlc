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

/**
 * The PCR-Delayer utility API is to be used in modules that treats incoming data asynchronously
 * and are supposed to bind a PCR event to their output.
 */

/**
 * Opaque PCR-Delayer internal state.
 */
typedef struct vlc_pcr_delayer vlc_pcr_delayer_t;

/**
 * Allocate and initialize a new pcr_delayer.
 *
 * \param max_delay The media time delay taken by the processing unit guarded by the delayer.
 * \param send_blocks Will be called by the delayer when sending blocks is appropriate.
 * \param send_pcr Will be called by the delayer when sending pcr is appropriate.
 *
 * \return An heap allocated pcr_delayer or NULL if the allocation failed.
 */
VLC_API vlc_pcr_delayer_t *vlc_pcr_delayer_New( vlc_tick_t max_delay,
                                                int ( *send_blocks )( void *, block_t * ),
                                                void ( *send_pcr )( void *, vlc_tick_t ) ) VLC_USED;

/**
 * Delete a pcr_delayer and discard its internal state.
 */
VLC_API void vlc_pcr_delayer_Delete( vlc_pcr_delayer_t * );

typedef enum {
    VLC_PCR_DELAYER_SUCCESS,
    VLC_PCR_DELAYER_ERROR,
    VLC_PCR_DELAYER_NO_DELAY, /**< */
} vlc_pcr_delayer_status_t;

/**
 * Signal a PCR event to the delayer.
 *
 * \note Passing the last received DTS is required to ease the delayer job when establishing a
 * PCR/DTS relationship.
 *
 * \param pcr The PCR value.
 * \param block_dts Last received data dts from the caller module.
 *
 * \return TODO
 */
VLC_API vlc_pcr_delayer_status_t vlc_pcr_delayer_SignalPCR( vlc_pcr_delayer_t *,
                                                            vlc_tick_t pcr,
                                                            vlc_tick_t block_dts );

/**
 * Signal that a block entered the asynchronous delaying unit.
 *
 * \param block The block (won't be treated as a block chain).
 *
 * \return TODO
 */
VLC_API vlc_pcr_delayer_status_t vlc_pcr_delayer_SignalEnteringBlock( vlc_pcr_delayer_t *,
                                                                      const block_t * ) ;

/**
 * Signal that a block left the asynchronous delaying unit.
 *
 * \param block The block (won't be treated as a block chain).
 *
 * \return TODO
 */
VLC_API vlc_pcr_delayer_status_t vlc_pcr_delayer_SignalLeavingBlock( vlc_pcr_delayer_t *,
                                                                     block_t * );

/**
 * Send the blocks and PCR that where cached by \ref vlc_pcr_delayer_SignalLeavingBlock
 * in the right order.
 *
 * This will use the callbacks passed to \ref vlc_pcr_delayer_New.
 * \param cb_data User defined data to be passed to the send callbacks.
 *
 * \return send_blocks_cb return status
 */
VLC_API int vlc_pcr_delayer_SendBlocksAndPCR( vlc_pcr_delayer_t *, void *cb_data );

/**
 * Send all cached PCR that doesn't have any prepending blocks attached to them.
 *
 * \param cb_data User defined data to be passed to the send callbacks.
 */
VLC_API void vlc_pcr_delayer_SendFrontPCRs( vlc_pcr_delayer_t *, void *cb_data );

/**
 * To be called when the delaying unit drains its internal state.
 *
 * This will discard currently tracked blocks and send the potentially delayed PCRs.
 *
 * \param cb_data User defined data to be passed to the send callbacks.
 */
VLC_API void vlc_pcr_delayer_Drain( vlc_pcr_delayer_t *, void *cb_data );

#if defined(__cplusplus)
}
#endif

#endif
