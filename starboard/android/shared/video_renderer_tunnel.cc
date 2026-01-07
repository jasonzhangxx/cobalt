// Copyright 2026 The Cobalt Authors. All Rights Reserved.
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

#include "starboard/android/shared/video_renderer_tunnel.h"

#include "starboard/common/log.h"
#include "starboard/common/media.h"
#include "starboard/common/string.h"
#include "starboard/common/time.h"
#include "starboard/shared/starboard/media/media_util.h"

namespace starboard::android::shared {

namespace {

using std::placeholders::_1;
using std::placeholders::_2;

const int64_t kSeekTimeoutInitialInterval = 500'000;  // 250ms
const int64_t kSeekTimeoutRetryInterval = 25'000;     // 25ms

}  // namespace

VideoRendererTunneled::VideoRendererTunneled(
    const VideoStreamInfo& video_stream_info,
    SbDrmSystem drm_system,
    int tunnel_mode_audio_session_id,
    bool force_big_endian_hdr_metadata,
    int max_video_input_size)
    : video_stream_info_(video_stream_info),
      drm_system_(static_cast<DrmSystem*>(drm_system)),
      tunnel_mode_audio_session_id_(tunnel_mode_audio_session_id),
      force_big_endian_hdr_metadata_(force_big_endian_hdr_metadata),
      max_video_input_size_(max_video_input_size),
      media_codec_feeder_(std::make_unique<AsyncMediaCodecInputFeeder>(
          drm_system_,
          std::bind(&VideoRendererTunneled::OnInputBufferEnqueued, this, _1),
          std::bind(&VideoRendererTunneled::OnMediaCodecFeederError, this, _1, _2))),
      // TODO: remove the hardecoded 1024
      video_frame_tracker_(std::make_unique<VideoFrameTracker>(1024)) {
        SB_DLOG(INFO) << "Creating VideoRendererTunneled with "<<video_stream_info_;
      }

VideoRendererTunneled::~VideoRendererTunneled() {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DLOG(INFO) << "Destroying VideoRendererTunneled with "<<video_stream_info_;
  media_codec_feeder_->Flush();
  TeardownMediaCodec();
}

void VideoRendererTunneled::Initialize(const ErrorCB& error_cb,
                                     const PrerolledCB& prerolled_cb,
                                     const EndedCB& ended_cb) {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(error_cb);
  SB_DCHECK(prerolled_cb);
  SB_DCHECK(ended_cb);
  SB_DCHECK(!error_cb_);
  SB_DCHECK(!prerolled_cb_);
  SB_DCHECK(!ended_cb_);

  error_cb_ = error_cb;
  prerolled_cb_ = prerolled_cb;
  ended_cb_ = ended_cb;

  // Keep the video surface until VideoRendererTunneled is released.
  video_surface_holder_ = std::make_unique<CallbackVideoSurfaceHolder>(
      std::bind(&VideoRendererTunneled::ReportError, this,
                kSbPlayerErrorCapabilityChanged,
                "Video surface has been destroyed."));
  InitializeMediaCodec();
}

int VideoRendererTunneled::GetDroppedFrames() const {
  return video_frame_tracker_->UpdateAndGetDroppedFrames();
}

void VideoRendererTunneled::WriteSamples(const InputBuffers& input_buffers) {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(!input_buffers.empty());
  for (const auto& input_buffer : input_buffers) {
    SB_DCHECK(input_buffer);
  }

  if (end_of_stream_written_) {
    SB_DLOG(WARNING) << "Ignore the samples written after EOS.";
    return;
  }

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }

  if (!first_input_written_) {
    first_input_written_ = true;

    // TODO: it's from legacy code, verify if it could still happen in C26.
    // If color metadata is present and is changed, re-create the codec with the
    // new metadata.
    const auto& color_metadata =
        input_buffers.front()->video_stream_info().color_metadata;
    if (video_stream_info_.color_metadata != color_metadata) {
      SB_LOG(WARNING) << "Color metadata changed ("
                      << video_stream_info_.color_metadata << ") -> ("
                      << color_metadata << ").";
      TeardownMediaCodec();
      video_stream_info_.color_metadata = color_metadata;
      InitializeMediaCodec();

      // TODO: after codec is re-created, we need to call seekTo before start
      // feeding.
      media_codec_feeder_->StartFeeding();
    }
  }

  // TODO: refine the loop
  for (const auto& input_buffer : input_buffers) {
    video_frame_tracker_->OnInputBuffer(input_buffer->timestamp());
  }
  media_codec_feeder_->EnqueueInputBatch(input_buffers);
}

void VideoRendererTunneled::WriteEndOfStream() {
  SB_DCHECK(BelongsToCurrentThread());

  if (end_of_stream_written_) {
    SB_DLOG(WARNING) << "Ignore the EOS written after EOS.";
    return;
  }

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }

  end_of_stream_written_ = true;

  if (!first_input_written_) {
    first_input_written_ = true;
  }

  media_codec_feeder_->EnqueueEndOfStream();

  // TODO: hanlde eos and call ended_cb_ properly
  ended_cb_();
}

void VideoRendererTunneled::Seek(int64_t seek_to_time) {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK_GE(seek_to_time, 0);

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }

#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "Start seeking.";
  seeking_start_at_ = CurrentMonotonicTime();
#endif  // TUNNEL_ENABLE_STATE_LOGGING

  if (first_input_written_) {
    FlushMediaCodec();
    // TODO: reset video frame tracker
    first_input_written_ = false;
    end_of_stream_written_ = false;
  }

  is_seeking_ = true;
  seeking_to_time_ = seek_to_time;
  video_frame_tracker_->Seek(seek_to_time);
  media_codec_bridge_->Seek(seek_to_time);
  // Start feeding after seekTo is called.
  media_codec_feeder_->StartFeeding();

  // TODO: verify if the fallback seek timeout is necessary.
  Schedule(std::bind(&VideoRendererTunneled::OnSeekTimeout, this),
           kSeekTimeoutInitialInterval);
}

bool VideoRendererTunneled::CanAcceptMoreData() const {
  SB_DCHECK(BelongsToCurrentThread());
  // TODO: replace the hardcoded 128.
  return !has_error_ && video_frame_tracker_->GetNumberPendingFrames() < 128;
}

bool Equal(const SbMediaMasteringMetadata& lhs,
           const SbMediaMasteringMetadata& rhs) {
  return memcmp(&lhs, &rhs, sizeof(SbMediaMasteringMetadata)) == 0;
}

// TODO: remove this function
bool IsIdentity(const SbMediaColorMetadata& color_metadata) {
  const SbMediaMasteringMetadata kEmptyMasteringMetadata = {};
  return color_metadata.primaries == kSbMediaPrimaryIdBt709 &&
         color_metadata.transfer == kSbMediaTransferIdBt709 &&
         color_metadata.matrix == kSbMediaMatrixIdBt709 &&
         color_metadata.range == kSbMediaRangeIdLimited &&
         Equal(color_metadata.mastering_metadata, kEmptyMasteringMetadata);
}

void VideoRendererTunneled::InitializeMediaCodec() {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(!media_codec_bridge_);

#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "Initialize MediaCodecBridge " << video_stream_info_;
#endif  // TUNNEL_ENABLE_STATE_LOGGING

  jobject j_output_surface = video_surface_holder_->AcquireVideoSurface();
  if (!j_output_surface) {
    // TODO: verify if reporting kSbPlayerErrorCapabilityChanged here could
    // cause more playback errors.
    ReportError(kSbPlayerErrorDecode, "Failed to find the video surface.");
    return;
  }

  std::string error_message;
  media_codec_bridge_ = MediaCodecBridge::CreateVideoMediaCodecBridge(
      video_stream_info_.codec, video_stream_info_.frame_width,
      video_stream_info_.frame_height,
      /*fps*/ 0, std::nullopt, std::nullopt,  // primary player only
      this, j_output_surface,
      drm_system_ ? drm_system_->GetMediaCrypto() : nullptr,
      IsIdentity(video_stream_info_.color_metadata)
          ? nullptr
          : &video_stream_info_.color_metadata,
      drm_system_ && drm_system_->require_secured_decoder(),
      /*require_software_codec*/ false, tunnel_mode_audio_session_id_,
      force_big_endian_hdr_metadata_, max_video_input_size_, &error_message);
  if (!media_codec_bridge_) {
    ReportError(kSbPlayerErrorDecode, error_message);
    return;
  }

  // Start the callbacks after |media_codec_bridge_| is received.
  // TODO: use Start() instad of Restart().
  if (!media_codec_bridge_->Restart()) {
    ReportError(kSbPlayerErrorDecode, "Failed to start video codec.");
    return;
  }
}

void VideoRendererTunneled::FlushMediaCodec() {
  SB_DCHECK(BelongsToCurrentThread());

  is_flushing_ = true;
  media_codec_feeder_->Flush();
  jint status = media_codec_bridge_->Flush();
  if (status != MEDIA_CODEC_OK) {
    SB_LOG(WARNING) << "Failed to flush MeidaCodec, destroying the codec.";
    TeardownMediaCodec();
  }
  is_flushing_ = false;

  if (media_codec_bridge_ && !media_codec_bridge_->Restart()) {
    // Failed to restart flushed MediaCodec.
    SB_LOG(WARNING) << "Failed to restart media codec, destroying the codec.";
    TeardownMediaCodec();
  }

  if (!media_codec_bridge_) {
    InitializeMediaCodec();
  }
}

void VideoRendererTunneled::TeardownMediaCodec() {
  SB_DCHECK(BelongsToCurrentThread());

  media_codec_bridge_->Stop();
  media_codec_bridge_.reset();

#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "MediaCodecBridge is teared down.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
}

void VideoRendererTunneled::TryToSignalPreroll() {
  if (is_seeking_.exchange(false)) {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "Video preroll takes "
                 << CurrentMonotonicTime() - seeking_start_at_
                 << " microseconds.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
    prerolled_cb_();
  }
}

void VideoRendererTunneled::OnSeekTimeout() {
  SB_DCHECK(BelongsToCurrentThread());

  if (!is_seeking_) {
    // Seek is done.
    return;
  }

  // TODO: replace the hardecoded 16
  if (video_frame_tracker_->GetNumberPendingFrames() < 16) {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(WARNING) << "Seek timed out. Try to start the playback anyway.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
    TryToSignalPreroll();
  } else {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(WARNING) << "Renderer is still waiting for more inputs.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
    Schedule(std::bind(&VideoRendererTunneled::OnSeekTimeout, this),
             kSeekTimeoutRetryInterval);
  }
}

void VideoRendererTunneled::ReportError(const SbPlayerError error,
                                      const std::string error_message) {
  SB_DCHECK(error_cb_);
  if (!has_error_.exchange(true)) {
    SB_LOG(ERROR) << "Unrecoverable error (video): " << error_message;
    // Try best to stop the pipeline to avoid more unexpected error.
    media_codec_feeder_->StopFeeding();
    error_cb_(error, error_message);
  }
}

void VideoRendererTunneled::OnInputBufferEnqueued(int64_t timestamp) {
  // TODO: add frame tracker
}

AsyncMediaCodecInputFeeder::ErrorAction VideoRendererTunneled::OnMediaCodecFeederError(
    MediaCodecStatus status,
    const std::string& message) {
  if (status == MEDIA_CODEC_NO_KEY) {
    return AsyncMediaCodecInputFeeder::ErrorAction::kRetry;
  } else if (status == MEDIA_CODEC_INSUFFICIENT_OUTPUT_PROTECTION) {
    // TODO: reduce the retry frequency when output is restricted.
    drm_system_->OnInsufficientOutputProtection();
    return AsyncMediaCodecInputFeeder::ErrorAction::kRetry;
  }
  ReportError(kSbPlayerErrorDecode, message);
  return AsyncMediaCodecInputFeeder::ErrorAction::kStop;
}

void VideoRendererTunneled::OnMediaCodecError(
    bool is_recoverable,
    bool is_transient,
    const std::string& diagnostic_info) {
  SB_LOG(WARNING) << "MediaCodecDecoder (video) encountered "
                  << (is_recoverable ? "recoverable, " : "unrecoverable, ")
                  << (is_transient ? "transient " : "intransient ")
                  << " error with message: " << diagnostic_info;
  // The callback may be called on a different thread and before |error_cb_| is
  // initialized.
  if (!is_transient) {
    ReportError(kSbPlayerErrorDecode,
                "OnMediaCodecError (tunneled_video): " + diagnostic_info +
                    (is_recoverable ? ", recoverable " : ", unrecoverable "));
  }
}

void VideoRendererTunneled::OnMediaCodecInputBufferAvailable(int buffer_index) {
  SB_DCHECK(media_codec_bridge_);

  // Prevent adding new input buffers to |media_codec_feeder_| during flush.
  if (!is_flushing_) {
    media_codec_feeder_->OnMediaCodecInputBufferAvailable(
        media_codec_bridge_.get(), buffer_index);
  } else {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(WARNING) << "Available input buffer(idx:" << buffer_index
                    << ") from MediaCodec was ignored during flushing";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
  }
}

void VideoRendererTunneled::OnMediaCodecOutputBufferAvailable(
    int buffer_index,
    int flags,
    int offset,
    int64_t presentation_time_us,
    int size) {
  SB_NOTREACHED();

  SB_LOG(ERROR) << "VideoRendererTunneled::OnMediaCodecOutputBufferAvailable";
}

void VideoRendererTunneled::OnMediaCodecOutputFormatChanged() {
  // TODO: verify if this callback could happen under tunnel mode.
  SB_LOG(ERROR) << "VideoRendererTunneled::OnMediaCodecOutputFormatChanged";
}

void VideoRendererTunneled::OnMediaCodecFrameRendered(int64_t frame_timestamp) {
#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "Received rendered frame (@" << frame_timestamp << ") at "
               << CurrentMonotonicTime() << ".";
#endif  // TUNNEL_ENABLE_STATE_LOGGING

  TryToSignalPreroll();
  video_frame_tracker_->OnFrameRendered(frame_timestamp);
}

void VideoRendererTunneled::OnMediaCodecFirstTunnelFrameReady() {
#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "Received first tunnel frame ready.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING

  TryToSignalPreroll();
}

}  // namespace starboard::android::shared
