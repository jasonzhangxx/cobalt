// Copyright 2016 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef STARBOARD_ANDROID_SHARED_VIDEO_RENDERER_TUNNEL_H_
#define STARBOARD_ANDROID_SHARED_VIDEO_RENDERER_TUNNEL_H_

#include <atomic>
#include <functional>

#include "starboard/android/shared/drm_system.h"
#include "starboard/android/shared/media_codec_bridge.h"
#include "starboard/android/shared/video_frame_tracker.h"
#include "starboard/android/shared/video_window.h"
#include "starboard/common/ref_counted.h"
#include "starboard/media.h"
#include "starboard/shared/internal_only.h"
#include "starboard/shared/starboard/player/filter/common.h"
#include "starboard/shared/starboard/player/filter/media_time_provider.h"
#include "starboard/shared/starboard/player/filter/video_renderer_internal.h"
#include "starboard/shared/starboard/player/input_buffer_internal.h"
#include "starboard/shared/starboard/player/job_queue.h"

#define TUNNEL_ENABLE_STATE_LOGGING 1

namespace starboard {

// TODO: move them out
class AsyncMediaCodecInputFeeder;
enum class ErrorAction {
  kRetry,  // Enqueue the current input again
  kStop,   // Stop the feeder entirely
};

class TunnelVideoRenderer : public VideoRenderer,
                            private MediaCodecBridge::Handler,
                            private JobQueue::JobOwner {
 public:
  // All of the functions are called on the PlayerWorker thread unless marked
  // otherwise.
  TunnelVideoRenderer(const VideoStreamInfo& video_stream_info,
                      SbDrmSystem drm_system,
                      int tunnel_mode_audio_session_id,
                      bool force_big_endian_hdr_metadata,
                      int max_video_input_size);
  ~TunnelVideoRenderer() override;

  // Video renderer functions
  void Initialize(const ErrorCB& error_cb,
                  const PrerolledCB& prerolled_cb,
                  const EndedCB& ended_cb) override;
  int GetDroppedFrames() const override;
  void WriteSamples(const InputBuffers& input_buffers) override;
  void WriteEndOfStream() override;
  void Seek(int64_t seek_to_time) override;
  bool IsEndOfStreamWritten() const override { return end_of_stream_written_; }
  bool CanAcceptMoreData() const override;
  // SetBounds() and GetCurrentDecodeTarget() are not used for tunnel mode.
  void SetBounds(int z_index, int x, int y, int width, int height) override {}
  SbDecodeTarget GetCurrentDecodeTarget() override { return nullptr; }

 private:
  void InitializeMediaCodec();
  void FlushMediaCodec();
  void TeardownMediaCodec();

  void TryToSignalPreroll();
  void OnSeekTimeout();

  void ReportError(const SbPlayerError error, const std::string error_message);

  // AsyncMediaCodecInputFeeder callbacks
  void OnInputBufferEnqueued(int64_t timestamp);
  ErrorAction OnMediaCodecFeederError(MediaCodecStatus status,
                                      const std::string& message);

  // MediaCodecBridge handler functions
  void OnMediaCodecError(bool is_recoverable,
                         bool is_transient,
                         const std::string& diagnostic_info) override;
  void OnMediaCodecInputBufferAvailable(int buffer_index) override;
  void OnMediaCodecOutputBufferAvailable(int buffer_index,
                                         int flags,
                                         int offset,
                                         int64_t presentation_time_us,
                                         int size) override;
  void OnMediaCodecOutputFormatChanged() override;
  void OnMediaCodecFrameRendered(int64_t frame_timestamp) override;
  void OnMediaCodecFirstTunnelFrameReady() override;

  ErrorCB error_cb_;
  PrerolledCB prerolled_cb_;
  EndedCB ended_cb_;

  VideoStreamInfo video_stream_info_;
  DrmSystem* drm_system_;
  const int tunnel_mode_audio_session_id_ = -1;
  const bool force_big_endian_hdr_metadata_;
  const int max_video_input_size_;

  std::unique_ptr<AsyncMediaCodecInputFeeder> media_codec_feeder_;
  std::unique_ptr<VideoFrameTracker> video_frame_tracker_;
  std::unique_ptr<CallbackVideoSurfaceHolder> video_surface_holder_;
  std::unique_ptr<MediaCodecBridge> media_codec_bridge_;

  // Our owner will attempt to seek to time 0 when playback begins.  In
  // general, seeking could require a full reset of the underlying decoder on
  // some platforms, so we make an effort to improve playback startup
  // performance by keeping track of whether we already have a fresh decoder,
  // and can thus avoid doing a full reset.
  bool first_input_written_ = false;
  bool end_of_stream_written_ = false;

  std::atomic_bool is_seeking_{false};
  int64_t seeking_to_time_ = 0;  // microseconds
  std::atomic_bool is_flushing_{false};
  std::atomic_bool has_error_{false};

#if TUNNEL_ENABLE_STATE_LOGGING
  int64_t seeking_start_at_;  // microseconds
#endif                        // TUNNEL_ENABLE_STATE_LOGGING
};

}  // namespace starboard

#endif  // STARBOARD_ANDROID_SHARED_VIDEO_RENDERER_TUNNEL_H_
