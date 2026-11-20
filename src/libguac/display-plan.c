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

#include "display-plan.h"
#include "display-priv.h"
#include "guacamole/assert.h"
#include "guacamole/client.h"
#include "guacamole/display.h"
#include "guacamole/fifo.h"
#include "guacamole/mem.h"
#include "guacamole/protocol.h"
#include "guacamole/rect.h"
#include "guacamole/socket.h"
#include "guacamole/timestamp.h"

#include <limits.h>
#include <string.h>
#include <cairo/cairo.h>

/**
 * Updates the dirty rect in the given cell to note that a horizontal line of
 * image data at the given location and having the given width has changed
 * since the last frame. A provided counter of the overall number of changed
 * cells is updated accordingly.
 *
 * @param layer
 *     The layer that changed.
 *
 * @param cell
 *     The cell containing the line of image data that changed.
 *
 * @param count
 *     A pointer to a counter that contains the current number of cells that
 *     have been marked as having changed since the last frame.
 *
 * @param x
 *     The X coordinate of the leftmost pixel of the horizontal line.
 *
 * @param y
 *     The Y coordinate of the leftmost pixel of the horizontal line.
 *
 * @param width
 *     The width of the line, in pixels.
 */
static void guac_display_plan_mark_dirty(guac_display_layer* layer,
        guac_display_layer_cell* cell, size_t* count, int x, int y,
        int width) {

    if (!cell->dirty_size) {
        guac_rect_init(&cell->dirty, x, y, width, 1);
        cell->dirty_size = width;
        (*count)++;
    }

    else {
        guac_rect dirty;
        guac_rect_init(&dirty, x, y, width, 1);
        guac_rect_extend(&cell->dirty, &dirty);
        cell->dirty_size += width;
    }

}

/**
 * Variant of memcmp() which specifically compares series of 32-bit quantities
 * and determines the overall location and length of the differences in the two
 * provided buffers. The length and location determined are the length and
 * location of the smallest contiguous series of 32-bit quantities that differ
 * between the buffers.
 *
 * @param buffer_a
 *     The first buffer to compare.
 *
 * @param buffer_b
 *     The buffer to compare with buffer_a.
 *
 * @param count
 *     The number of 32-bit quantities in each buffer.
 *
 * @param pos
 *     A pointer to a size_t that should receive the offset of the difference,
 *     if the two buffers turn out to contain different data. The value of the
 *     size_t will only be modified if at least one difference is found.
 *
 * @return
 *     The number of 32-bit quantities after and including the offset returned
 *     via pos that are different between buffer_a and buffer_b, or zero if
 *     there are no such differences.
 */
static size_t guac_display_memcmp(const uint32_t* restrict buffer_a,
        const uint32_t* restrict buffer_b, size_t count, size_t* pos) {

    /* Locate first difference between the buffers, if any */
    size_t first = 0;
    while (first < count) {

        if (*(buffer_a++) != *(buffer_b++))
            break;

        first++;

    }

    /* If we reached the end without finding any differences, no need to search
     * further - the buffers are identical */
    if (first >= count)
        return 0;

    /* Search through all remaining values in the buffers for the last
     * difference (which may be identical to the first) */
    size_t last = first;
    size_t offset = first + 1;
    while (offset < count) {

        if (*(buffer_a++) != *(buffer_b++))
            last = offset;

        offset++;

    }

    /* Final difference found - provide caller with the starting offset and
     * length (in 32-bit quantities) of differences */
    *pos = first;
    return last - first + 1;

}

guac_display_plan* PFW_LFR_guac_display_plan_create(guac_display* display) {

    guac_display_layer* current;
    guac_timestamp frame_end = guac_timestamp_current();
    size_t op_count = 0;

    /* Loop through each layer, searching for modified regions */
    current = display->pending_frame.layers;
    while (current != NULL) {

        /* Skip processing any layers whose buffers have been replaced with
         * NULL (this is intentionally allowed to ensure references to external
         * buffers can be safely removed if necessary, even before guac_display
         * is freed) */
        if (current->pending_frame.buffer == NULL) {
            GUAC_ASSERT(current->pending_frame.buffer_is_external);
            current = current->pending_frame.next;
            continue;
        }

        /* Check only within layer dirty region, skipping the layer if
         * unmodified. This pass should reset and refine that region, but
         * otherwise rely on proper reporting of modified regions by callers of
         * the open/close layer functions. */
        guac_rect dirty = current->pending_frame.dirty;
        if (guac_rect_is_empty(&dirty)) {
            current = current->pending_frame.next;
            continue;
        }

        /* The default layer is sent as video instead, when enabled (its dirty
         * region is still committed to the last frame, where the video
         * encoder reads it) */
        if (display->video != NULL && current == display->default_layer) {
            current = current->pending_frame.next;
            continue;
        }

        /* Flush any outstanding Cairo operations before directly accessing buffer */
        guac_display_layer_cairo_context* cairo_context = &(current->pending_frame_cairo_context);
        if (cairo_context->surface != NULL)
            cairo_surface_flush(cairo_context->surface);

        /* Re-align the dirty rect with nearest multiple of 64 to ensure each
         * step of the dirty rect refinement loop starts at the topmost
         * boundary of a cell */
        guac_rect_align(&dirty, GUAC_DISPLAY_CELL_SIZE_EXPONENT);

        guac_rect pending_frame_bounds = {
            .left = 0,
            .top = 0,
            .right = current->pending_frame.width,
            .bottom = current->pending_frame.height
        };

        /* Limit size of dirty rect by bounds of backing surface for pending
         * frame ONLY (bounds checks against the last frame are performed
         * within the loop such that everything outside the bounds of the last
         * frame is considered dirty) */
        guac_rect_constrain(&dirty, &pending_frame_bounds);

        const unsigned char* flushed_row = GUAC_DISPLAY_LAYER_STATE_CONST_BUFFER(current->last_frame, dirty);
        unsigned char* buffer_row = GUAC_DISPLAY_LAYER_STATE_MUTABLE_BUFFER(current->pending_frame, dirty);

        guac_display_layer_cell* cell_row = current->pending_frame_cells
            + guac_mem_ckd_mul_or_die(dirty.top / GUAC_DISPLAY_CELL_SIZE, current->pending_frame_cells_width)
            + dirty.left / GUAC_DISPLAY_CELL_SIZE;

        /* Loop through the rough modified region, refining the dirty rects of
         * each cell to more accurately contain only what has actually changed
         * since last frame */ 
        current->pending_frame.dirty = (guac_rect) { 0 };
        for (int corner_y = dirty.top; corner_y < dirty.bottom; corner_y += GUAC_DISPLAY_CELL_SIZE) {

            int height = GUAC_DISPLAY_CELL_SIZE;
            if (corner_y + height > dirty.bottom)
                height = dirty.bottom - corner_y;

            /* Iteration through the pending_frame_cells array and the image
             * buffer is a bit complex here, as the pending_frame_cells array
             * contains cells that represent 64x64 regions, while the image
             * buffers contain absolutely all pixels. The outer loop goes
             * through just the pending cells, while the following loop goes
             * through the Y coordinates that make up that cell. */

            for (int y_off = 0; y_off < height; y_off++) {

                /* At this point, we need to loop through the horizontal
                 * dimension, comparing the 64-pixel rows of image data in the
                 * current line (corner_y + y_off) that are in each applicable
                 * cell. We jump forward by one cell for each comparison. */

                int y = corner_y + y_off;

                guac_display_layer_cell* current_cell = cell_row;
                uint32_t* current_flushed = (uint32_t*) flushed_row;
                uint32_t* current_buffer = (uint32_t*) buffer_row;
                for (int corner_x = dirty.left; corner_x < dirty.right; corner_x += GUAC_DISPLAY_CELL_SIZE) {

                    int width = GUAC_DISPLAY_CELL_SIZE;
                    if (corner_x + width > dirty.right)
                        width = dirty.right - corner_x;

                    /* This SHOULD be impossible, as corner_x would need to
                     * somehow be outside the bounds of the dirty rect, which
                     * would have failed the loop condition earlier) */
                    GUAC_ASSERT(width >= 0);

                    /* Any line that is completely outside the bounds of the
                     * previous frame is dirty (nothing to compare against) */
                    if (y >= current->last_frame.height || corner_x >= current->last_frame.width) {
                        guac_display_plan_mark_dirty(current, current_cell, &op_count, corner_x, y, width);
                        guac_rect_extend(&current->pending_frame.dirty, &current_cell->dirty);
                    }

                    /* All other regions must be processed further to determine
                     * what portion is dirty */
                    else {

                        /* Only the pixels that are within the bounds of BOTH
                         * the last_frame and pending_frame are directly
                         * comparable. Others are inherently dirty by virtue of
                         * being outside the bounds of last_frame */
                        int comparable_width = width;
                        if (corner_x + comparable_width > current->last_frame.width)
                            comparable_width = current->last_frame.width - corner_x;

                        /* It is impossible for this value to be negative
                         * because of the last_frame bounds checks that occur
                         * in the if block prior to this else block */
                        GUAC_ASSERT(comparable_width >= 0);

                        /* Any region outside the right edge of the previous frame is dirty */
                        if (width > comparable_width) {
                            guac_display_plan_mark_dirty(current, current_cell, &op_count, corner_x + comparable_width, y, width - comparable_width);
                            guac_rect_extend(&current->pending_frame.dirty, &current_cell->dirty);
                        }

                        /* Mark the relevant region of the cell as dirty if the
                         * current 64-pixel line has changed in any way */
                        size_t length, pos;
                        if ((length = guac_display_memcmp(current_buffer, current_flushed, comparable_width, &pos)) != 0) {
                            guac_display_plan_mark_dirty(current, current_cell, &op_count, corner_x + pos, y, length);
                            guac_rect_extend(&current->pending_frame.dirty, &current_cell->dirty);
                        }

                    }

                    current_flushed += GUAC_DISPLAY_CELL_SIZE;
                    current_buffer += GUAC_DISPLAY_CELL_SIZE;
                    current_cell++;

                }

                flushed_row += current->last_frame.buffer_stride;
                buffer_row += current->pending_frame.buffer_stride;

            }

            cell_row += current->pending_frame_cells_width;

        }

        current = current->pending_frame.next;

    }

    /* If no layer has been modified, there's no need to create a plan */
    if (!op_count)
        return NULL;

    guac_display_plan* plan = guac_mem_alloc(sizeof(guac_display_plan));
    plan->display = display;
    plan->frame_end = frame_end;
    plan->length = op_count;
    plan->ops = guac_mem_alloc(plan->length, sizeof(guac_display_plan_operation));

    /* Convert the dirty rectangles stored in each layer's cells to individual
     * image operations for later optimization */
    size_t added_ops = 0;
    guac_display_plan_operation* current_op = plan->ops;
    current = display->pending_frame.layers;
    while (current != NULL) {

        guac_display_layer_cell* cell = current->pending_frame_cells;
        for (int y = 0; y < current->pending_frame_cells_height; y++) {
            for (int x = 0; x < current->pending_frame_cells_width; x++) {

                if (cell->dirty_size) {

                    /* The overall number of ops that we try to add via these
                     * nested loops should always exactly align with the
                     * anticipated count produced earlier and therefore not
                     * overrun the ops array at any point unless there is a bug
                     * in the way the original operation count was calculated */
                    GUAC_ASSERT(added_ops < op_count);

                    current_op->layer = current;
                    current_op->type = GUAC_DISPLAY_PLAN_OPERATION_IMG;
                    current_op->dest = cell->dirty;
                    current_op->dirty_size = cell->dirty_size;
                    current_op->last_frame = cell->last_frame;
                    current_op->current_frame = frame_end;
                    current_op->lossless = 0;

                    cell->related_op = current_op;
                    cell->dirty_size = 0;
                    cell->last_frame = frame_end;

                    current_op++;
                    added_ops++;

                }
                else
                    cell->related_op = NULL;

                cell++;

            }
        }

        current = current->pending_frame.next;

    }

    /* At this point, the number of operations added should exactly match the
     * predicted quantity */
    GUAC_ASSERT(added_ops == op_count);

    return plan;

}

void guac_display_plan_free(guac_display_plan* plan) {
    guac_mem_free(plan->ops);
    guac_mem_free(plan);
}

void guac_display_plan_apply(guac_display_plan* plan) {

    guac_display* display = plan->display;
    guac_client* client = display->client;
    guac_display_plan_operation* op = plan->ops;

    /* Do not allow worker threads to move forward with image encoding until
     * AFTER the non-image instructions have finished being written */
    guac_fifo_lock(&display->ops);

    /* Immediately send instructions for all updates that do not involve
     * significant processing (do not involve encoding anything). This allows
     * us to use the worker threads solely for encoding, reducing contention
     * between the threads. */
    for (int i = 0; i < plan->length; i++) {

        guac_display_layer* display_layer = op->layer;
        switch (op->type) {

            case GUAC_DISPLAY_PLAN_OPERATION_COPY:
                guac_protocol_send_copy(client->socket, op->src.layer_rect.layer,
                        op->src.layer_rect.rect.left, op->src.layer_rect.rect.top,
                        guac_rect_width(&op->src.layer_rect.rect), guac_rect_height(&op->src.layer_rect.rect),
                        GUAC_COMP_OVER, display_layer->layer, op->dest.left, op->dest.top);
                break;

            case GUAC_DISPLAY_PLAN_OPERATION_RECT:

                guac_protocol_send_rect(client->socket, display_layer->layer,
                        op->dest.left, op->dest.top, guac_rect_width(&op->dest), guac_rect_height(&op->dest));

                int alpha = (op->src.color & 0xFF000000) >> 24;
                int red   = (op->src.color & 0x00FF0000) >> 16;
                int green = (op->src.color & 0x0000FF00) >> 8;
                int blue  = (op->src.color & 0x000000FF);

                /* Clear before drawing if layer is not opaque (transparency
                 * will not be copied correctly otherwise) */
                if (!display_layer->opaque) {
                    guac_protocol_send_cfill(client->socket, GUAC_COMP_ROUT, display_layer->layer, 0x00, 0x00, 0x00, 0xFF);
                    guac_protocol_send_cfill(client->socket, GUAC_COMP_OVER, display_layer->layer, red, green, blue, alpha);
                }
                else
                    guac_protocol_send_cfill(client->socket, GUAC_COMP_OVER, display_layer->layer, red, green, blue, 0xFF);

                break;

            /* Simply ignore and drop NOP */
            case GUAC_DISPLAY_PLAN_OPERATION_NOP:
                break;

            /* All other operations should be handled by the workers */
            default:
                guac_fifo_enqueue(&display->ops, op);
                break;

        }

        op++;

    }

    guac_fifo_unlock(&display->ops);

}

/**
 * Returns whether the given operation may result in lossy image data on the
 * client: an IMG operation at a framerate where the worker threads may choose
 * JPEG or lossy WebP, or a COPY operation (whose source may itself be lossy).
 * This errs on the side of caution, as a needless refresh costs only one
 * extra lossless image, while a missed one leaves the region blurry.
 */
static int guac_display_plan_op_may_be_lossy(const guac_display_plan_operation* op) {

    if (op->type == GUAC_DISPLAY_PLAN_OPERATION_COPY)
        return 1;

    if (op->type != GUAC_DISPLAY_PLAN_OPERATION_IMG || op->lossless)
        return 0;

    /* Same framerate calculation as the worker threads */
    int framerate = INT_MAX;
    if (op->current_frame > op->last_frame)
        framerate = 1000 / (op->current_frame - op->last_frame);

    return framerate >= GUAC_DISPLAY_JPEG_FRAMERATE;

}

void PFW_guac_display_plan_mark_lossy(guac_display_plan* plan) {

    guac_display_plan_operation* op = plan->ops;
    for (size_t i = 0; i < plan->length; i++, op++) {

        guac_display_layer* layer = op->layer;
        if (layer == NULL || layer->pending_frame.lossless
                || !guac_display_plan_op_may_be_lossy(op)
                || guac_rect_is_empty(&op->dest))
            continue;

        int left = op->dest.left / GUAC_DISPLAY_CELL_SIZE;
        int top = op->dest.top / GUAC_DISPLAY_CELL_SIZE;
        int right = (op->dest.right - 1) / GUAC_DISPLAY_CELL_SIZE;
        int bottom = (op->dest.bottom - 1) / GUAC_DISPLAY_CELL_SIZE;

        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right >= (int) layer->pending_frame_cells_width)
            right = layer->pending_frame_cells_width - 1;
        if (bottom >= (int) layer->pending_frame_cells_height)
            bottom = layer->pending_frame_cells_height - 1;

        for (int y = top; y <= bottom; y++) {
            guac_display_layer_cell* cell = layer->pending_frame_cells
                + (size_t) y * layer->pending_frame_cells_width + left;
            for (int x = left; x <= right; x++, cell++)
                cell->lossy = 1;
        }

    }

}

guac_display_plan* PFW_LFW_guac_display_plan_add_refresh(guac_display* display,
        guac_display_plan* plan, int* added) {

    guac_timestamp now = guac_timestamp_current();
    guac_timestamp next_due = 0;
    size_t count = 0;

    *added = 0;

    /* PASS 1: Count the runs of horizontally-adjacent cells that are due
     * (each run becomes one operation), and find when the next cell that is
     * not yet due will be */
    for (guac_display_layer* current = display->pending_frame.layers;
            current != NULL; current = current->pending_frame.next) {

        if (current->pending_frame.buffer == NULL || current->pending_frame_cells == NULL)
            continue;

        guac_display_layer_cell* cell = current->pending_frame_cells;
        for (size_t y = 0; y < current->pending_frame_cells_height; y++) {
            int in_run = 0;
            for (size_t x = 0; x < current->pending_frame_cells_width; x++, cell++) {

                int due = 0;
                if (cell->lossy) {
                    guac_timestamp cell_due = cell->last_frame + GUAC_DISPLAY_LOSSLESS_REFRESH_DELAY;
                    if (cell_due <= now)
                        due = 1;
                    else if (!next_due || cell_due < next_due)
                        next_due = cell_due;
                }

                if (due && !in_run)
                    count++;

                in_run = due;

            }
        }

    }

    display->lossless_refresh_due = next_due;

    if (!count)
        return plan;

    /* Allocate space for the new operations at the end of the plan */
    size_t first;
    if (plan == NULL) {
        plan = guac_mem_alloc(sizeof(guac_display_plan));
        plan->display = display;
        plan->frame_end = now;
        plan->length = count;
        plan->ops = guac_mem_alloc(count, sizeof(guac_display_plan_operation));
        first = 0;
    }
    else {
        first = plan->length;
        plan->length = guac_mem_ckd_add_or_die(plan->length, count);
        plan->ops = guac_mem_realloc(plan->ops, plan->length, sizeof(guac_display_plan_operation));
    }

    /* PASS 2: Build one lossless IMG operation per run of due cells */
    guac_display_plan_operation* op = plan->ops + first;
    size_t built = 0;

    for (guac_display_layer* current = display->pending_frame.layers;
            current != NULL; current = current->pending_frame.next) {

        if (current->pending_frame.buffer == NULL || current->pending_frame_cells == NULL)
            continue;

        guac_rect bounds = {
            .left = 0,
            .top = 0,
            .right = current->last_frame.width,
            .bottom = current->last_frame.height
        };

        guac_display_layer_cell* cell = current->pending_frame_cells;
        for (size_t y = 0; y < current->pending_frame_cells_height; y++) {

            int run_start = -1;
            for (size_t x = 0; x <= current->pending_frame_cells_width; x++) {

                int due = 0;
                if (x < current->pending_frame_cells_width) {
                    due = cell->lossy && cell->last_frame + GUAC_DISPLAY_LOSSLESS_REFRESH_DELAY <= now;
                    if (due)
                        cell->lossy = 0;
                    cell++;
                }

                if (due && run_start < 0)
                    run_start = x;

                /* End of a run: emit it */
                else if (!due && run_start >= 0) {

                    guac_rect rect;
                    guac_rect_init(&rect,
                            run_start * GUAC_DISPLAY_CELL_SIZE,
                            y * GUAC_DISPLAY_CELL_SIZE,
                            (x - run_start) * GUAC_DISPLAY_CELL_SIZE,
                            GUAC_DISPLAY_CELL_SIZE);
                    guac_rect_constrain(&rect, &bounds);

                    GUAC_ASSERT(built < count);

                    /* Cells beyond the edge of a shrunk layer have nothing to
                     * refresh */
                    if (guac_rect_is_empty(&rect))
                        op->type = GUAC_DISPLAY_PLAN_OPERATION_NOP;
                    else {
                        op->type = GUAC_DISPLAY_PLAN_OPERATION_IMG;
                        guac_rect_extend(&current->last_frame.dirty, &rect);
                        (*added)++;
                    }

                    op->layer = current;
                    op->dest = rect;
                    op->dirty_size = (size_t) guac_rect_width(&rect) * guac_rect_height(&rect);
                    op->last_frame = now;
                    op->current_frame = now;
                    op->lossless = 1;

                    op++;
                    built++;
                    run_start = -1;

                }

            }
        }

    }

    GUAC_ASSERT(built == count);
    return plan;

}
