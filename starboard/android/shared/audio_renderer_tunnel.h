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

#ifndef STARBOARD_ANDROID_SHARED_AUDIO_RENDERER_TUNNEL_H_
#define STARBOARD_ANDROID_SHARED_AUDIO_RENDERER_TUNNEL_H_

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "starboard/android/shared/audio_decoder.h"
#include "starboard/common/log.h"
#include "starboard/media.h"
#include "starboard/shared/internal_only.h"
#include "starboard/shared/starboard/player/decoded_audio_internal.h"
#include "starboard/shared/starboard/player/filter/audio_decoder_internal.h"
#include "starboard/shared/starboard/player/filter/audio_renderer_internal.h"
#include "starboard/shared/starboard/player/filter/media_time_provider.h"
#include "starboard/shared/starboard/player/input_buffer_internal.h"
#include "starboard/shared/starboard/player/job_queue.h"
#include "starboard/shared/starboard/player/job_thread.h"
#include "starboard/types.h"

#define TUNNEL_ENABLE_STATE_LOGGING 1

namespace starboard::android::shared {

using ::starboard::shared::starboard::player::filter::ErrorCB;
using ::starboard::shared::starboard::player::filter::PrerolledCB;
using ::starboard::shared::starboard::player::filter::EndedCB;

using ::starboard::shared::starboard::media::AudioStreamInfo;
using ::starboard::shared::starboard::player::filter::AudioRenderer;
using ::starboard::shared::starboard::player::filter::MediaTimeProvider;
using ::starboard::shared::starboard::player::JobQueue;
using ::starboard::shared::starboard::player::InputBuffer;
using ::starboard::shared::starboard::player::InputBuffers;

class AudioRendererTunnel : public AudioRenderer,
                         private JobQueue::JobOwner {
 public:
  AudioRendererTunnel(std::unique_ptr<AudioDecoder> decoder,
                   const AudioStreamInfo& audio_stream_info,
                   int tunnel_mode_audio_session_id);
  ~AudioRendererTunnel() override;

  // Audio renderer functions.
  void Initialize(const ErrorCB& error_cb,
                  const PrerolledCB& prerolled_cb,
                  const EndedCB& ended_cb) override;
  void WriteSamples(const InputBuffers& input_buffers) override;
  void WriteEndOfStream() override;
  void SetVolume(double volume) override;
  // TODO: Remove the eos state querying functions and their tests.
  bool IsEndOfStreamWritten() const override;
  bool IsEndOfStreamPlayed() const override;
  bool CanAcceptMoreData() const override;

  void Seek(int64_t seek_to_time);

 private:

  void ReportError(const SbPlayerError error, const std::string error_message);

  ErrorCB error_cb_;
  PrerolledCB prerolled_cb_;
  EndedCB ended_cb_;

  std::unique_ptr<AudioDecoder> audio_decoder_;
  const AudioStreamInfo audio_stream_info_;
  const int tunnel_mode_audio_session_id_;

  // std::unique_ptr<AudioTrackBridge> audio_track_bridge_;
  // std::unique_ptr<JobThread> audio_track_thread_;

  // Our owner will attempt to seek to time 0 when playback begins.  In
  // general, seeking could require a full reset of the underlying decoder on
  // some platforms, so we make an effort to improve playback startup
  // performance by keeping track of whether we already have a fresh decoder,
  // and can thus avoid doing a full reset.
  bool first_input_written_ = false;
  bool end_of_stream_written_ = false;

  std::atomic_bool has_error_{false};
};

}  // namespace starboard::android::shared

#endif  // STARBOARD_ANDROID_SHARED_AUDIO_RENDERER_TUNNEL_H_