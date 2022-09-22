/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libplacebo/renderer.h>

#ifndef LIBPLACEBO_FRAME_QUEUE_H
#define LIBPLACEBO_FRAME_QUEUE_H

// This file contains an abstraction layer for automatically turning a
// conceptual stream of (frame, pts) pairs, as emitted by a decoder or filter
// graph, into a `pl_frame_mix` suitable for `pl_render_image_mix`.
//
// This API ensures that minimal work is performed (e.g. only mapping frames
// that are actually required), while also satisfying the requirements
// of any configured frame mixer.

enum pl_queue_status {
    QUEUE_OK,       // success
    QUEUE_EOF,      // no more frames are available
    QUEUE_MORE,     // more frames needed, but not (yet) available
    QUEUE_ERR = -1, // some unknown error occurred while retrieving frames
};

struct pl_source_frame {
    // The frame's presentation timestamp, in seconds relative to the first
    // frame. These must be monotonically increasing for subsequent frames.
    // To implement a discontinuous jump, users must explicitly reset the
    // frame queue with `pl_queue_reset` and restart from PTS 0.0.
    float pts;

    // Abstract frame data itself. To allow mapping frames only when they're
    // actually needed, frames use a lazy representation. The provided
    // callbacks will be invoked to interface with it.
    void *frame_data;

    // This will be called to map the frame to the GPU, only if needed.
    //
    // `tex` is a pointer to an array of 4 texture objects (or NULL), which
    // *may* serve as backing storage for the texture being mapped. These are
    // intended to be recreated by `map`, e.g. using `pl_tex_recreate` or
    // `pl_upload_plane` as appropriate. They will be managed internally by
    // `pl_queue` and destroyed at some unspecified future point in time.
    //
    // Note: If `map` fails, it will not be retried, nor will `discard` be run.
    // The user should clean up state in this case.
    bool (*map)(const struct pl_gpu *gpu, const struct pl_tex **tex,
                const struct pl_source_frame *src, struct pl_frame *out_frame);

    // If present, this will be called on frames that are done being used by
    // `pl_queue`. This may be useful to e.g. unmap textures backed by external
    // APIs such as hardware decoders. (Optional)
    void (*unmap)(const struct pl_gpu *gpu, struct pl_frame *frame,
                  const struct pl_source_frame *src);

    // This function will be called for frames that are deemed unnecessary
    // (e.g. never became visible) and should instead be cleanly freed.
    // (Optional)
    void (*discard)(const struct pl_source_frame *src);
};

// Create a new, empty frame queue.
//
// It's highly recommended to fully render a single frame with `pts == 0.0`,
// and flush the GPU pipeline with `pl_gpu_finish`, prior to starting the timed
// playback loop.
struct pl_queue *pl_queue_create(const struct pl_gpu *gpu);
void pl_queue_destroy(struct pl_queue **queue);

// Explicitly clear the queue. This is essentially equivalent to destroying
// and recreating the queue, but preserves any internal memory allocations.
void pl_queue_reset(struct pl_queue *queue);

// Explicitly push a frame. This is an alternative way to feed the frame queue
// with incoming frames, the other method being the asynchronous callback
// specified as `pl_queue_params.get_frame`. Both methods may be used
// simultaneously, although providing `get_frame` is recommended since it
// avoids the risk of the queue underrunning.
//
// When no more frames are available, call this function with `frame == NULL`
// to indicate EOF and begin draining the frame queue.
void pl_queue_push(struct pl_queue *queue, const struct pl_source_frame *frame);

struct pl_queue_params {
    // The PTS of the frame that will be rendered. This should be set to the
    // timestamp (in seconds) of the next vsync, relative to the initial frame.
    //
    // These must be monotonically increasing. To implement a discontinuous
    // jump, users must explicitly reset the frame queue with `pl_queue_reset`
    // and restart from PTS 0.0.
    float pts;

    // The radius of the configured mixer. This should be set to the value
    // as returned by `pl_frame_mix_radius`.
    float radius;

    // The estimated duration of a vsync, in seconds. This will only be used as
    // a hint, the true value will be estimated by comparing `pts` timestamps
    // between calls to `pl_queue_update`. (Optional)
    float vsync_duration;

    // The estimated duration of a frame, in seconds. This will only be used as
    // an initial hint, the true value will be estimated by comparing `pts`
    // timestamps between source frames. (Optional)
    float frame_duration;

    // This callback will be used to pull new frames from the decoder. It may
    // block if needed. The user is responsible for setting appropriate time
    // limits and/or returning and interpreting QUEUE_MORE as sensible.
    //
    // Providing this callback is entirely optional. Users can instead choose
    // to manually feed the frame queue with new frames using `pl_queue_push`.
    enum pl_queue_status (*get_frame)(struct pl_source_frame *out_frame,
                                      const struct pl_queue_params *params);
    void *priv;
};

// Advance the frame queue's internal state to the target timestamp. Any frames
// which are no longer needed (i.e. too far in the past) are automatically
// unmapped and evicted. Any future frames which are needed to fill the queue
// must either have been pushed in advance, or will be requested using the
// provided `get_frame` callback.
//
// This function may fail with QUEUE_MORE, in which case the user must
// ensure more frames are available and then re-run this function with
// the same parameters.
//
// The resulting mix of frames in `out_mix` will represent the neighbourhood of
// the target timestamp, and can be passed to `pl_render_image_mix` as-is.
//
// Note: `out_mix` will only remain valid until the next call to `pl_queue_*`.
enum pl_queue_status pl_queue_update(struct pl_queue *queue,
                                     struct pl_frame_mix *out_mix,
                                     const struct pl_queue_params *params);

#endif // LIBPLACEBO_FRAME_QUEUE_H
