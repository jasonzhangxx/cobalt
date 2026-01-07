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

#include "starboard/android/shared/async_media_codec_input_feeder.h"

#include <sched.h>

#include "base/android/jni_android.h"
#include "base/android/scoped_java_ref.h"
#include "starboard/common/log.h"
#include "starboard/common/string.h"
#include "starboard/shared/starboard/media/media_util.h"

//TODO: remove TUNNEL_ENABLE_STATE_LOGGING
#define TUNNEL_ENABLE_STATE_LOGGING 1

namespace starboard::android::shared {

namespace {

using ::starboard::shared::starboard::player::InputBuffer;

using base::android::AttachCurrentThread;
using base::android::ScopedJavaLocalRef;

}  // namespace

struct AsyncMediaCodecInputFeeder::PendingInput {
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

AsyncMediaCodecInputFeeder::AsyncMediaCodecInputFeeder(
    DrmSystem* drm_system,
    const OnInputBufferEnqueuedCallback& input_buffer_enqueued_cb,
    const OnErrorCallback& error_cb)
    : drm_system_(drm_system),
      input_buffer_enqueued_cb_(input_buffer_enqueued_cb),
      error_cb_(error_cb),
      job_thread_(new JobThread("media_codec_input_feeder")) {
  SB_DCHECK(input_buffer_enqueued_cb_);
  SB_DCHECK(error_cb_);
}

AsyncMediaCodecInputFeeder::~AsyncMediaCodecInputFeeder() {
#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "Destroying AsyncMediaCodecInputFeeder.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
  // |job_thread_| will be set to nullptr before deleting the underlying object and |job_thread_| is used
  // in some jobs, so we need to ensure all jobs are flushed before reset it.
  Flush();
  job_thread_.reset();
#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "AsyncMediaCodecInputFeeder is destroyed.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
}

void AsyncMediaCodecInputFeeder::EnqueueCodecConfig(const std::vector<uint8_t>& codec_config) {
  job_thread_->Schedule(
      std::bind(&AsyncMediaCodecInputFeeder::DoEnqueueInput,
                this, PendingInput(codec_config)));
}

void AsyncMediaCodecInputFeeder::EnqueueInputBatch(const InputBuffers& input_buffers) {
  for (const auto& input_buffer : input_buffers) {
    job_thread_->Schedule(
        std::bind(&AsyncMediaCodecInputFeeder::DoEnqueueInput,
                  this, PendingInput(input_buffer)));
  }
}

void AsyncMediaCodecInputFeeder::EnqueueEndOfStream() {
  job_thread_->Schedule(
      std::bind(&AsyncMediaCodecInputFeeder::DoEnqueueInput,
                this, PendingInput(PendingInput::kWriteEndOfStream)));
}

void AsyncMediaCodecInputFeeder::OnMediaCodecInputBufferAvailable(MediaCodecBridge* media_codec_bridge,
                                      int buffer_index) {
  job_thread_->Schedule(std::bind(
      &AsyncMediaCodecInputFeeder::DoOnMediaCodecInputBufferAvailable, this,
      media_codec_bridge, buffer_index));
}

void AsyncMediaCodecInputFeeder::StartFeeding() {
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

void AsyncMediaCodecInputFeeder::StopFeeding() {
#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "StartFeeding";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
  is_feeding_paused_ = true;
}

// Enqueue requests before calling Flush() will be flushed.
void AsyncMediaCodecInputFeeder::Flush() {
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

void AsyncMediaCodecInputFeeder::DoEnqueueInput(const PendingInput& input) {
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

void AsyncMediaCodecInputFeeder::DoOnMediaCodecInputBufferAvailable(MediaCodecBridge* media_codec_bridge,
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

void AsyncMediaCodecInputFeeder::DoProcessInput() {
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

void AsyncMediaCodecInputFeeder::DoFlush() {
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

void AsyncMediaCodecInputFeeder::HandleError(MediaCodecStatus error_status,
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

void AsyncMediaCodecInputFeeder::TryStartProcessInputJob() {
  SB_DCHECK(job_thread_->BelongsToCurrentThread());

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

}  // namespace starboard::android::shared
