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

#ifndef STARBOARD_ANDROID_SHARED_ASYNC_MEDIA_CODEC_INPUT_FEDDER_H_
#define STARBOARD_ANDROID_SHARED_ASYNC_MEDIA_CODEC_INPUT_FEDDER_H_

#include <atomic>
#include <deque>
#include <functional>

#include "starboard/android/shared/drm_system.h"
#include "starboard/android/shared/media_codec_bridge.h"
#include "starboard/common/ref_counted.h"
#include "starboard/shared/internal_only.h"
#include "starboard/shared/starboard/player/filter/common.h"
#include "starboard/shared/starboard/player/input_buffer_internal.h"
#include "starboard/shared/starboard/player/job_thread.h"

namespace starboard::android::shared {

using ::starboard::shared::starboard::player::InputBuffers;

using android::shared::DrmSystem;
using android::shared::MediaCodecStatus;
using android::shared::MediaCodecBridge;

using ::starboard::shared::starboard::player::JobQueue;
using ::starboard::shared::starboard::player::JobThread;

class AsyncMediaCodecInputFeeder {
 public:

  enum class ErrorAction {
    kRetry,  // Enqueue the current input again
    kStop,   // Stop the feeder entirely
  };

  using OnInputBufferEnqueuedCallback = std::function<void(int64_t)>;
  using OnErrorCallback =
      std::function<ErrorAction(MediaCodecStatus, const std::string&)>;

  AsyncMediaCodecInputFeeder(
      DrmSystem* drm_system,
      const OnInputBufferEnqueuedCallback& input_buffer_enqueued_cb,
      const OnErrorCallback& error_cb);
  ~AsyncMediaCodecInputFeeder();

  void EnqueueCodecConfig(const std::vector<uint8_t>& codec_config);
  void EnqueueInputBatch(const InputBuffers& input_buffers);
  void EnqueueEndOfStream();

  void OnMediaCodecInputBufferAvailable(MediaCodecBridge* media_codec_bridge,
                                        int buffer_index);

  void StartFeeding();
  void StopFeeding();

  // Enqueue requests before calling Flush() will be flushed. Calling Flush() will pause feeding.
  void Flush();

 private:
  AsyncMediaCodecInputFeeder(const AsyncMediaCodecInputFeeder&) = delete;
  AsyncMediaCodecInputFeeder& operator=(const AsyncMediaCodecInputFeeder&) =
      delete;

  struct PendingInput;

  void DoEnqueueInput(const PendingInput& input);
  void DoOnMediaCodecInputBufferAvailable(MediaCodecBridge* media_codec_bridge,
                                          int buffer_index);
  void DoProcessInput();
  void DoFlush();


  void HandleError(MediaCodecStatus error_status,
                 const std::string& error_message);
  void TryStartProcessInputJob();

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

}  // namespace starboard::android::shared 

#endif  // STARBOARD_ANDROID_SHARED_ASYNC_MEDIA_CODEC_INPUT_FEDDER_H_
