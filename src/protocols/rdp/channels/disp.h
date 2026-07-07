/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef GUAC_RDP_CHANNELS_DISP_H
#define GUAC_RDP_CHANNELS_DISP_H

/*
 * Multi-monitor model
 * -------------------
 *
 * Each RDP connection tracks an array of `guac_rdp_disp_monitor` entries.
 * Monitor 0 is always primary; entries 1..N-1 are secondaries opened by
 * the client. The client communicates monitor adds/resizes/closes via the
 * Guacamole `size` opcode, extended in the multi-monitor protocol to
 * carry per-monitor positioning:
 *
 *     size <width> <height> [<x_position>] [<top_offset>] [<left_offset>]
 *
 *   - x_position is the monitor's index in the layout. 0 = primary.
 *   - top_offset is the monitor's vertical placement relative to the
 *     primary's top edge (negative = above, positive = below).
 *   - left_offset is the monitor's horizontal placement, supplied by the
 *     client to permit non-linear layouts (left-of-primary, gaps between
 *     monitors). When the client omits it (older clients predating
 *     non-linear multimon support), the server falls back to a
 *     left-to-right row, computing each monitor's left_offset as the
 *     cumulative sum of widths of monitors with a lower x_position.
 *
 * A request with width == 0 or height == 0 at a non-primary x_position
 * closes that monitor.
 *
 * When the underlying RDP desktop is resized, guac_rdp_gdi_desktop_resize
 * (in gdi.c) broadcasts the current layout to all connected users via a
 * `set` instruction on the default layer with parameter
 * GUAC_PROTOCOL_LAYER_PARAMETER_MULTIMON_LAYOUT, containing a JSON object
 * mapping monitor id to {left, top, width, height}.
 */

#include "settings.h"

#include <freerdp/client/disp.h>
#include <freerdp/freerdp.h>
#include <guacamole/client.h>
#include <guacamole/timestamp.h>

/**
 * The minimum value for width or height, in pixels.
 */
#define GUAC_RDP_DISP_MIN_SIZE 200

/**
 * The maximum value for width or height, in pixels.
 */
#define GUAC_RDP_DISP_MAX_SIZE 8192

/**
 * The minimum amount of time that must elapse between display size updates,
 * in milliseconds.
 */
#define GUAC_RDP_DISP_UPDATE_INTERVAL 500

/**
 * For a MULTI-monitor layout, the amount of quiet time (no further size
 * requests) that must elapse before the accumulated layout is sent to the
 * server, in milliseconds. A multi-monitor layout change arrives as a burst
 * of one size request per monitor; without this settle window the first
 * changed monitor could trigger a SendMonitorLayout carrying a half-applied
 * (gap/overlap) layout before the rest of the burst lands. Waiting briefly for
 * the burst to settle collapses it into a single atomic PDU. Single-monitor
 * resizes are exempt
 * (this is only consulted when monitors_count > 1).
 */
#define GUAC_RDP_DISP_SETTLE_INTERVAL 75

/**
 * Monitor properties (size, position).
 */
typedef struct guac_rdp_disp_monitor {

    /**
     * The last requested screen width, in pixels.
     */
    int requested_width;

    /**
     * The last requested screen height, in pixels.
     */
    int requested_height;

    /*
     * The position of the monitor relative to the other monitors.
     */
    int x_position;

    /**
     * The offset of the monitor from the top of the layout, in pixels.
     */
    int top_offset;

    /**
     * The offset of the monitor from the left of the layout, in pixels.
     */
    int left_offset;

} guac_rdp_disp_monitor;

/**
 * Display size update module.
 */
typedef struct guac_rdp_disp {

    /**
     * The guac_client instance handling the relevant RDP connection.
     */
    guac_client* client;

    /**
     * Display control interface.
     */
    DispClientContext* disp;

    /**
     * The timestamp of the last display update request, or 0 if no request
     * has been sent yet.
     */
    guac_timestamp last_request;

    /**
     * The timestamp of the most recent incoming size/close request (i.e. the
     * last mutation of the monitor layout from the client), or 0 if none. Used
     * to detect when a multi-monitor request burst has settled — see
     * GUAC_RDP_DISP_SETTLE_INTERVAL.
     */
    guac_timestamp last_set_size;

    /**
     * Monitor properties (size, position). 
     */
    guac_rdp_disp_monitor* monitors;

    /**
     * The number of monitors.
     */
    int monitors_count;

    /**
     * The number of monitors most recently reported to the connected client
     * via a multimon-layout broadcast. Used to detect when a *new* monitor
     * is added so the local cursor can be reset, while avoiding cursor
     * flicker on every desktop resize. Protected by message_lock.
     */
    int reported_monitors_count;

    /**
     * Whether the size has changed and the RDP connection must be closed and
     * reestablished.
     */
    int reconnect_needed;

    /**
     * At least one monitor has been resized or moved and the display must be
     * resized.
     */
    bool resize_needed;

} guac_rdp_disp;

/**
 * Allocates a new display update module, which will ultimately control the
 * display update channel once connected.
 *
 * @param client
 *     The guac_client instance handling the relevant RDP connection.
 *
 * @return
 *     A newly-allocated display update module.
 */
guac_rdp_disp* guac_rdp_disp_alloc(guac_client* client);

/**
 * Frees the resources associated with support for the RDP Display Update
 * channel. Only resources specific to Guacamole are freed. Resources specific
 * to FreeRDP's handling of the Display Update channel will be freed by
 * FreeRDP. If no resources are currently allocated for Display Update support,
 * this function has no effect.
 *
 * @param disp
 *     The display update module to free.
 */
void guac_rdp_disp_free(guac_rdp_disp* disp);

/**
 * Decides whether a default-layer `copy` operation must be decomposed
 * into a fresh image transmission because it crosses between monitors.
 *
 * Designed to be registered via guac_display_set_should_decompose_copy_handler()
 * with the guac_rdp_client as the closure. When multiple monitors are
 * configured, a single canvas-side `copy` opcode that takes pixels from
 * one monitor's region and writes them to another monitor's region
 * cannot be represented on the connected client (whose per-window
 * canvas is clipped to one monitor's portion of the full desktop), so
 * the server transmits the destination region as fresh pixel data
 * instead.
 *
 * Returns false if monitors_count <= 1 (single monitor — `copy` is
 * always safe). Otherwise probes the corners of the source and
 * destination rectangles and returns true if any of them falls in a
 * different monitor than the one containing the source's top-left
 * corner.
 *
 * Thread-safe: acquires the same message_lock used to serialize
 * disp->monitors mutations. Note that the verdict is a single point-
 * in-time snapshot — see the docstring on
 * guac_display_should_decompose_copy_handler in display.h for the
 * caller-visible staleness implications.
 *
 * @param closure
 *     A guac_rdp_client* cast to void*.
 *
 * @param src_x
 *     The X coordinate of the source region's top-left corner.
 *
 * @param src_y
 *     The Y coordinate of the source region's top-left corner.
 *
 * @param width
 *     The width of the copied region, in pixels.
 *
 * @param height
 *     The height of the copied region, in pixels.
 *
 * @param dst_x
 *     The X coordinate of the destination region's top-left corner.
 *
 * @param dst_y
 *     The Y coordinate of the destination region's top-left corner.
 *
 * @return
 *     true if the copy crosses monitor boundaries and should be
 *     decomposed, false otherwise.
 */
bool guac_rdp_disp_should_decompose_copy(void* closure,
        int src_x, int src_y, int width, int height,
        int dst_x, int dst_y);

/**
 * Adds FreeRDP's "disp" plugin to the list of dynamic virtual channel plugins
 * to be loaded by FreeRDP's "drdynvc" plugin. The context of the plugin will
 * automatically be associated with the guac_rdp_disp instance pointed to by the
 * current guac_rdp_client. The plugin will only be loaded once the "drdynvc"
 * plugin is loaded. The "disp" plugin ultimately adds support for the Display
 * Update channel.
 *
 * If failures occur, messages noting the specifics of those failures will be
 * logged, and the RDP side of Display Update support will not be functional.
 *
 * This MUST be called within the PreConnect callback of the freerdp instance
 * for Display Update support to be loaded.
 *
 * @param context
 *     The rdpContext associated with the active RDP session.
 */
void guac_rdp_disp_load_plugin(rdpContext* context);

/**
 * Requests a resize / addition / closure of a monitor slot in the
 * multi-monitor layout. The update may then be sent immediately to the RDP
 * server, or delayed (e.g. coalesced with the rest of a multi-monitor request
 * burst) until the server has had time to settle.
 *
 * @param disp
 *     The display update module to update.
 *
 * @param settings
 *     The RDP connection's settings (used for resize-method dispatch). These
 *     settings will be automatically adjusted to match the new screen size.
 *
 * @param rdp_inst
 *     The active FreeRDP instance, or NULL if not yet connected.
 *
 * @param width
 *     The desired width of the monitor, in remote pixels. Pass 0 to
 *     close a non-primary monitor. Due to the restrictions of the RDP
 *     display update channel, a non-zero width is constrained to the
 *     range of 200 through 8192 inclusive and rounded down to the
 *     nearest even number.
 *
 * @param height
 *     The desired height of the monitor, in remote pixels. Pass 0 to
 *     close a non-primary monitor. Due to the same restrictions, a
 *     non-zero height is constrained to the range of 200 through 8192
 *     inclusive.
 *
 * @param x_position
 *     The monitor's slot index in the layout (0 = primary).
 *
 * @param top_offset
 *     The monitor's vertical offset from the primary's top edge, in
 *     remote pixels. Negative values are allowed.
 *
 * @param left_offset
 *     The monitor's horizontal offset within the combined desktop, in
 *     remote pixels. Negative values are allowed. Pass INT_MIN to
 *     indicate "not provided"; the implementation will then fall back
 *     to a horizontal-row layout where each monitor's left_offset is
 *     the cumulative sum of widths of monitors with a lower x_position.
 *     Older clients that pre-date non-linear multimon support omit the
 *     5th wire argument and so always trigger the fallback.
 */
void guac_rdp_disp_set_size(guac_rdp_disp* disp, guac_rdp_settings* settings,
        freerdp* rdp_inst, int width, int height,
        int x_position, int top_offset, int left_offset);

/**
 * Sends an actual display update request to the RDP server based on previous
 * calls to guac_rdp_disp_set_size(). If an update was recently sent, the
 * update may be delayed until a future call to this function. If the RDP
 * session has not yet been established, the request will be delayed until the
 * session exists.
 *
 * @param disp
 *     The display update module which should track the update request.
 *
 * @param settings
 *     The RDP client settings associated with the current or pending RDP
 *     session. These settings will be automatically adjusted to match the new
 *     screen size.
 *
 * @param rdp_inst
 *     The FreeRDP instance associated with the current or pending RDP session,
 *     if any. If no RDP session is active, this should be NULL.
 */
void guac_rdp_disp_update_size(guac_rdp_disp* disp,
        guac_rdp_settings* settings, freerdp* rdp_inst);

/**
 * Builds a JSON description of the current monitor layout (keyed by monitor
 * position, each {left, top, width, height}) and sends it to connected
 * clients as a multimon-layout layer parameter. Secondary monitor windows
 * use this to size and position their canvases. Acquires message_lock
 * internally to read the monitor array safely, and sends unlocked.
 *
 * Must be called whenever the committed monitor layout changes — both on
 * desktop-dimension changes (from gdi_desktop_resize) and on layout changes
 * that leave the bounding-box size unchanged (from guac_rdp_disp_update_size
 * after SendMonitorLayout), since the latter produce no DesktopResize.
 *
 * @param disp
 *     The display update module whose monitor layout should be broadcast.
 */
void guac_rdp_disp_broadcast_monitor_layout(guac_rdp_disp* disp);

/**
 * Signals the given display update module that the requested reconnect has
 * been performed.
 *
 * @param disp
 *     The display update module that should be signaled regarding the state
 *     of reconnection.
 */
void guac_rdp_disp_reconnect_complete(guac_rdp_disp* disp);

/**
 * Returns whether a full RDP reconnect is required for display update changes
 * to take effect.
 *
 * @param disp
 *     The display update module that should be checked to determine whether a
 *     reconnect is required.
 *
 * @return
 *     Non-zero if a reconnect is needed, zero otherwise.
 */
int guac_rdp_disp_reconnect_needed(guac_rdp_disp* disp);

#endif

