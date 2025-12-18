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

#include "starboard/android/shared/video_renderer_tunnel.h"

#include <sched.h>

#include <deque>

#include "base/android/jni_android.h"
#include "base/android/scoped_java_ref.h"
#include "starboard/common/log.h"
#include "starboard/common/media.h"
#include "starboard/common/string.h"
#include "starboard/common/time.h"
#include "starboard/shared/starboard/media/media_util.h"
#include "starboard/shared/starboard/player/job_thread.h"

namespace starboard::android::shared {

using ::starboard::shared::starboard::player::JobThread;

namespace {

using base::android::AttachCurrentThread;
using base::android::ScopedJavaLocalRef;
using std::placeholders::_1;
using std::placeholders::_2;

const int64_t kSeekTimeoutInitialInterval = 500'000;  // 250ms
const int64_t kSeekTimeoutRetryInterval = 25'000;     // 25ms

}  // namespace

// TODO: move it out of this file
class AsyncMediaCodecInputFeeder {
 public:
  using OnInputBufferEnqueuedCallback = std::function<void(int64_t)>;
  using OnErrorCallback =
      std::function<ErrorAction(MediaCodecStatus, const std::string&)>;

  AsyncMediaCodecInputFeeder(
      DrmSystem* drm_system,
      const OnInputBufferEnqueuedCallback input_buffer_enqueued_cb,
      const OnErrorCallback error_cb)
      : drm_system_(drm_system),
        input_buffer_enqueued_cb_(input_buffer_enqueued_cb),
        error_cb_(error_cb),
        job_thread_(new JobThread("media_codec_input_feeder")) {
    SB_DCHECK(input_buffer_enqueued_cb_);
    SB_DCHECK(error_cb_);
  }

  ~AsyncMediaCodecInputFeeder() {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "Destroying AsyncMediaCodecInputFeeder.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
    is_destroying_ = true;
    // TODO: remove all pending jobs instead of quick skipping all jobs by
    // |is_destroying_|.
    job_thread_.reset();
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "AsyncMediaCodecInputFeeder is destroyed.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
  }

  void EnqueueCodecConfig(const std::vector<uint8_t>& codec_config) {
    job_thread_->Schedule(
        std::bind(&AsyncMediaCodecInputFeeder::DoEnqueueInput,
                  this, PendingInput(codec_config)));
  }

  void EnqueueInputBatch(const InputBuffers& input_buffers) {
    for (const auto& input_buffer : input_buffers) {
      job_thread_->Schedule(
          std::bind(&AsyncMediaCodecInputFeeder::DoEnqueueInput,
                    this, PendingInput(input_buffer)));
    }
  }

  void EnqueueEndOfStream() {
    job_thread_->Schedule(
        std::bind(&AsyncMediaCodecInputFeeder::DoEnqueueInput,
                  this, PendingInput(PendingInput::kWriteEndOfStream)));
  }

  void OnMediaCodecInputBufferAvailable(MediaCodecBridge* media_codec_bridge,
                                        int buffer_index) {
    job_thread_->Schedule(std::bind(
        &AsyncMediaCodecInputFeeder::DoOnMediaCodecInputBufferAvailable, this,
        media_codec_bridge, buffer_index));
  }

  void StartFeeding() {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "StartFeeding";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
    if (is_feeding_paused_ == false) {
      return;
    }
    is_feeding_paused_ = false;
    job_thread_->Schedule(
        std::bind(&AsyncMediaCodecInputFeeder::TryStartProcessInputJob, this));
  }

  void StopFeeding() {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "StartFeeding";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
    is_feeding_paused_ = true;
  }

  // Enqueue requests before calling Flush() will be flushed.
  void Flush() {
    // TODO: optimize performance for clean feeder.
    is_feeding_paused_ = true;
    if (job_thread_->BelongsToCurrentThread()) {
      DoFlush();
    } else {
      // TODO: remove all pending jobs instead of quick skipping all jobs by
      // |is_destroying_|.
      is_destroying_ = true;
      job_thread_->ScheduleAndWait(
          std::bind(&AsyncMediaCodecInputFeeder::DoFlush, this));
      is_destroying_ = false;
    }
  }

 private:
  AsyncMediaCodecInputFeeder(const AsyncMediaCodecInputFeeder&) = delete;
  AsyncMediaCodecInputFeeder& operator=(const AsyncMediaCodecInputFeeder&) =
      delete;

  struct PendingInput {
    enum Type {
      kWriteCodecConfig,
      kWriteInputBuffer,
      kWriteEndOfStream,
    };

    explicit PendingInput(const std::vector<uint8_t>& codec_config)
        : type(kWriteCodecConfig), codec_config(codec_config) {
      SB_DCHECK(!this->codec_config.empty());
    }
    explicit PendingInput(const scoped_refptr<InputBuffer>& input_buffer)
        : type(kWriteInputBuffer), input_buffer(input_buffer) {}
    explicit PendingInput(Type type) : type(type) {
      SB_DCHECK(type == kWriteEndOfStream);
    }

    // Helper functions
    const void* data() const {
      if (type == kWriteCodecConfig) {
        return codec_config.data();
      } else if (type == kWriteInputBuffer) {
        return input_buffer->data();
      }
      return nullptr;
    }

    size_t size() const {
      if (type == kWriteCodecConfig) {
        return codec_config.size();
      } else if (type == kWriteInputBuffer) {
        return input_buffer->size();
      }
      return 0;
    }

    Type type;
    const scoped_refptr<InputBuffer> input_buffer;
    const std::vector<uint8_t> codec_config;
  };

  void DoEnqueueInput(const PendingInput& input) {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "DoEnqueueInput";
#endif  // TUNNEL_ENABLE_STATE_LOGGING

    if (is_destroying_) {
      return;
    }

    pending_inputs_.push_back(input);
    TryStartProcessInputJob();
  }

  void DoOnMediaCodecInputBufferAvailable(MediaCodecBridge* media_codec_bridge,
                                          int buffer_index) {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "DoOnMediaCodecInputBufferAvailable";
#endif  // TUNNEL_ENABLE_STATE_LOGGING

    if (is_destroying_) {
      return;
    }

    if (!media_codec_bridge_) {
      media_codec_bridge_ = media_codec_bridge;
    } else if (media_codec_bridge_ != media_codec_bridge) {
      // This is a rare corner case that |media_codec_bridge| changes, which
      // means there're dirty callbacks during flushing. In that case, we should
      // clear MediaCodec buffers.
      SB_LOG(WARNING) << "Feeder received buffers from a new MediaCodec, "
                         "removing all dirty buffers.";
      media_codec_input_buffers_.clear();
      media_codec_bridge_ = media_codec_bridge;
    }

    media_codec_input_buffers_.push_back(buffer_index);
    TryStartProcessInputJob();
  }

  void DoProcessInput() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());
    SB_DCHECK(!pending_inputs_.empty() && !media_codec_input_buffers_.empty());

#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "DoProcessInput";
#endif  // TUNNEL_ENABLE_STATE_LOGGING

    process_input_job_token_.ResetToInvalid();

    if (is_destroying_) {
      return;
    }

    const PendingInput& input = pending_inputs_.front();
    int media_codec_input_buffer_index = media_codec_input_buffers_.front();

    // TODO: retry would re-write the input buffer again. Optimization needed.
    if (input.size() > 0) {
      ScopedJavaLocalRef<jobject> byte_buffer(
          media_codec_bridge_->GetInputBuffer(media_codec_input_buffer_index));
      if (byte_buffer.is_null()) {
        // This could be a rare corner case that MediaCodec buffer is from dirty
        // callbacks during MediaCodec flushing. Remove the MediaCodec buffer
        // and try again.
        SB_LOG(WARNING) << "MediaCodec buffer is null, discarding the buffer.";
        media_codec_input_buffers_.pop_front();
        TryStartProcessInputJob();
        return;
      }

      JNIEnv* env = AttachCurrentThread();
      jint capacity = env->GetDirectBufferCapacity(byte_buffer.obj());
      if (capacity < static_cast<int>(input.size())) {
        auto error_message = FormatString(
            "Unable to write to MediaCodec buffer, input buffer size (%d) is"
            " greater than |byte_buffer.capacity()| (%d).",
            input.size(), static_cast<int>(capacity));
        HandleError(MEDIA_CODEC_ERROR, error_message);
        return;
      }

      void* address = env->GetDirectBufferAddress(byte_buffer.obj());
      memcpy(address, input.data(), input.size());
    }

    // Return immediately between time consuming works to optimize destroying
    // performance.
    if (is_destroying_) {
      return;
    }

    const jint kNoOffset = 0;
    const jlong kNoPts = 0;
    const jint kNoBufferFlags = 0;

    jint status;
    if (drm_system_ && !drm_system_->IsReady()) {
      status = MEDIA_CODEC_NO_KEY;
    } else if (input.type == PendingInput::kWriteCodecConfig) {
      status = media_codec_bridge_->QueueInputBuffer(
          media_codec_input_buffer_index, kNoOffset, input.size(), kNoPts,
          BUFFER_FLAG_CODEC_CONFIG, false);
    } else if (input.type == PendingInput::kWriteInputBuffer) {
      if (drm_system_ && input.input_buffer->drm_info()) {
        status = media_codec_bridge_->QueueSecureInputBuffer(
            media_codec_input_buffer_index, kNoOffset,
            *input.input_buffer->drm_info(), input.input_buffer->timestamp(),
            false);
      } else {
        status = media_codec_bridge_->QueueInputBuffer(
            media_codec_input_buffer_index, kNoOffset, input.size(),
            input.input_buffer->timestamp(), kNoBufferFlags, false);
      }
    } else {
      status = media_codec_bridge_->QueueInputBuffer(
          media_codec_input_buffer_index, kNoOffset, 0, kNoPts,
          BUFFER_FLAG_END_OF_STREAM, false);
    }

    if (status != MEDIA_CODEC_OK) {
      HandleError(static_cast<MediaCodecStatus>(status),
                  "Unable to enqueue input buffer.");
    } else {
      pending_inputs_.pop_front();
      media_codec_input_buffers_.pop_front();
    }

    TryStartProcessInputJob();
  }

  void DoFlush() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "DoFlush";
#endif  // TUNNEL_ENABLE_STATE_LOGGING

    if (process_input_job_token_.is_valid()) {
      job_thread_->RemoveJobByToken(process_input_job_token_);
      process_input_job_token_.ResetToInvalid();
    }
    pending_inputs_.clear();
    media_codec_input_buffers_.clear();
    media_codec_bridge_ = nullptr;
  }

  void HandleError(MediaCodecStatus error_status,
                   const std::string& error_message) {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

    ErrorAction action = error_cb_(error_status, error_message);
    switch (action) {
      case ErrorAction::kRetry:
        SB_LOG(INFO) << "Feeder encountered error: " << error_message
                     << ", will try again after a delay.";
        sched_yield();
        break;
      case ErrorAction::kStop:
        is_feeding_paused_ = true;
        SB_LOG(INFO) << "Feeder encountered error: " << error_message
                     << ", will stop the feeder.";
        break;
    }
  }

  void TryStartProcessInputJob() {
    if (process_input_job_token_.is_valid()) {
      // There's already an enqueued process input job.
      return;
    }
    if (pending_inputs_.empty()) {
      // There's no pending input.
      return;
    }
    if (media_codec_input_buffers_.empty()) {
      // There's no available MediaCodec input buffer.
      return;
    }
    if (is_feeding_paused_ || is_destroying_) {
      return;
    }
    process_input_job_token_ = job_thread_->Schedule(
        std::bind(&AsyncMediaCodecInputFeeder::DoProcessInput, this));
  }

  DrmSystem* drm_system_;
  const OnInputBufferEnqueuedCallback input_buffer_enqueued_cb_;
  const OnErrorCallback error_cb_;

  std::atomic_bool is_feeding_paused_{true};
  std::atomic_bool is_destroying_{false};

  // |pending_inputs_|, |media_codec_input_buffers_| and
  // |process_input_job_token_| are accessed only from |job_thread_|.
  MediaCodecBridge* media_codec_bridge_ = nullptr;
  std::deque<PendingInput> pending_inputs_;
  std::deque<int> media_codec_input_buffers_;
  JobQueue::JobToken process_input_job_token_;

  std::unique_ptr<JobThread> job_thread_;
};

TunnelVideoRenderer::TunnelVideoRenderer(
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
          std::bind(&TunnelVideoRenderer::OnInputBufferEnqueued, this, _1),
          std::bind(&TunnelVideoRenderer::OnMediaCodecFeederError,
                    this,
                    _1,
                    _2))),
      // TODO: remove the hardecoded 1024
      video_frame_tracker_(std::make_unique<VideoFrameTracker>(1024)) {}

TunnelVideoRenderer::~TunnelVideoRenderer() {
  SB_DCHECK(BelongsToCurrentThread());

  TeardownMediaCodec();
}

void TunnelVideoRenderer::Initialize(const ErrorCB& error_cb,
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

  // TODO: hanlde eos and call ended_cb_!!

  // Keep the video surface until TunnelVideoRenderer is released.
  video_surface_holder_ = std::make_unique<CallbackVideoSurfaceHolder>(
      std::bind(&TunnelVideoRenderer::ReportError, this,
                kSbPlayerErrorCapabilityChanged,
                "Video surface has been destroyed."));
  InitializeMediaCodec();
}

int TunnelVideoRenderer::GetDroppedFrames() const {
  return video_frame_tracker_->UpdateAndGetDroppedFrames();
}

void TunnelVideoRenderer::WriteSamples(const InputBuffers& input_buffers) {
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

void TunnelVideoRenderer::WriteEndOfStream() {
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
}

void TunnelVideoRenderer::Seek(int64_t seek_to_time) {
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
  Schedule(std::bind(&TunnelVideoRenderer::OnSeekTimeout, this),
           kSeekTimeoutInitialInterval);
}

bool TunnelVideoRenderer::CanAcceptMoreData() const {
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

void TunnelVideoRenderer::InitializeMediaCodec() {
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

void TunnelVideoRenderer::FlushMediaCodec() {
  SB_DCHECK(BelongsToCurrentThread());

  is_flushing_ = true;
  media_codec_feeder_->Flush();
  jint status = media_codec_bridge_->Flush();
  if (status != MEDIA_CODEC_OK) {
    SB_LOG(WARNING) << "Failed to flush MeidaCodec, destroying the codec.";
    media_codec_bridge_->Stop();
    media_codec_bridge_.reset();
  }
  is_flushing_ = false;

  if (media_codec_bridge_ && !media_codec_bridge_->Restart()) {
    // Failed to restart flushed MediaCodec.
    SB_LOG(WARNING) << "Failed to restart media codec, destroying the codec.";
    media_codec_bridge_->Stop();
    media_codec_bridge_.reset();
  }

  if (!media_codec_bridge_) {
    InitializeMediaCodec();
  }
}

void TunnelVideoRenderer::TeardownMediaCodec() {
  SB_DCHECK(BelongsToCurrentThread());

  is_flushing_ = true;
  media_codec_feeder_->Flush();
  media_codec_bridge_->Stop();
  media_codec_bridge_.reset();
  is_flushing_ = false;

#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "MediaCodecBridge is teared down.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
}

void TunnelVideoRenderer::TryToSignalPreroll() {
  if (is_seeking_.exchange(false)) {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "Video preroll takes "
                 << CurrentMonotonicTime() - seeking_start_at_
                 << " microseconds.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
    prerolled_cb_();
  }
}

void TunnelVideoRenderer::OnSeekTimeout() {
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
    Schedule(std::bind(&TunnelVideoRenderer::OnSeekTimeout, this),
             kSeekTimeoutRetryInterval);
  }
}

void TunnelVideoRenderer::ReportError(const SbPlayerError error,
                                      const std::string error_message) {
  SB_DCHECK(error_cb_);
  if (!has_error_.exchange(true)) {
    SB_LOG(ERROR) << "Unrecoverable error: " << error_message;
    // Try best to stop the pipeline to avoid more unexpected error.
    media_codec_feeder_->StopFeeding();
    error_cb_(error, error_message);
  }
}

void TunnelVideoRenderer::OnInputBufferEnqueued(int64_t timestamp) {
  // TODO: add frame tracker
}

ErrorAction TunnelVideoRenderer::OnMediaCodecFeederError(
    MediaCodecStatus status,
    const std::string& message) {
  if (status == MEDIA_CODEC_NO_KEY) {
    return ErrorAction::kRetry;
  } else if (status == MEDIA_CODEC_INSUFFICIENT_OUTPUT_PROTECTION) {
    // TODO: reduce the retry frequency when output is restricted.
    drm_system_->OnInsufficientOutputProtection();
    return ErrorAction::kRetry;
  }
  ReportError(kSbPlayerErrorDecode, message);
  return ErrorAction::kStop;
}

void TunnelVideoRenderer::OnMediaCodecError(
    bool is_recoverable,
    bool is_transient,
    const std::string& diagnostic_info) {
  SB_LOG(WARNING) << "MediaCodecDecoder encountered "
                  << (is_recoverable ? "recoverable, " : "unrecoverable, ")
                  << (is_transient ? "transient " : "intransient ")
                  << " error with message: " << diagnostic_info;
  // The callback may be called on a different thread and before |error_cb_| is
  // initialized.
  if (!is_transient) {
    ReportError(kSbPlayerErrorDecode,
                "OnMediaCodecError (tunnel_video): " + diagnostic_info +
                    (is_recoverable ? ", recoverable " : ", unrecoverable "));
  }
}

void TunnelVideoRenderer::OnMediaCodecInputBufferAvailable(int buffer_index) {
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

void TunnelVideoRenderer::OnMediaCodecOutputBufferAvailable(
    int buffer_index,
    int flags,
    int offset,
    int64_t presentation_time_us,
    int size) {
  SB_NOTREACHED();

  SB_LOG(ERROR) << "TunnelVideoRenderer::OnMediaCodecOutputBufferAvailable";
}

void TunnelVideoRenderer::OnMediaCodecOutputFormatChanged() {
  // TODO: verify if this callback could happen under tunnel mode.
  SB_LOG(ERROR) << "TunnelVideoRenderer::OnMediaCodecOutputFormatChanged";
}

void TunnelVideoRenderer::OnMediaCodecFrameRendered(int64_t frame_timestamp) {
#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "Received rendered frame (@" << frame_timestamp << ") at "
               << CurrentMonotonicTime() << ".";
#endif  // TUNNEL_ENABLE_STATE_LOGGING

  TryToSignalPreroll();
  video_frame_tracker_->OnFrameRendered(frame_timestamp);
}

void TunnelVideoRenderer::OnMediaCodecFirstTunnelFrameReady() {
#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "Received first tunnel frame ready.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING

  TryToSignalPreroll();
}

}  // namespace starboard::android::shared
