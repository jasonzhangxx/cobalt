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

class AudioTrackWrapper;
// TODO: move BufferHealth to right place.
enum class BufferHealth {
  kUnderrun,  // Buffered data is not enough to keep playing.
  kLow, //  buffered data < low_watermark
  kHealthy, // low_watermark <= buffered data <= high_watermark
  kFull,  // buffered data > high_watermark
};

class AudioRendererTunneled : public AudioRenderer,
                            public MediaTimeProvider,
                         private JobQueue::JobOwner {
 public:
  //TODO: try writing audio directly into AudioTrack.
  AudioRendererTunneled(const AudioStreamInfo& audio_stream_info,
                    std::unique_ptr<::starboard::shared::starboard::player::filter::AudioDecoder> decoder,
                   int tunnel_mode_audio_session_id);
  ~AudioRendererTunneled() override;

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

  void Play() override;
  void Pause() override;
  void SetPlaybackRate(double playback_rate) override;
  void Seek(int64_t seek_to_time) override;
  int64_t GetCurrentMediaTime(bool* is_playing,
                              bool* is_eos_played,
                              bool* is_underflow,
                              double* playback_rate) override;

 private:
  void InitializeAudioTrack();
  void TeardownAudioTrack();

  void TryToSignalPreroll();
  void UpdateAudioTrackPlayingState();
  void ReportError(const SbPlayerError error, const std::string error_message);

  // Audio decoder callbacks
  void OnDecoderConsumed();
  void OnDecoderOutput();

  // AudioTrack callback
  void OnBufferHealthChanged(BufferHealth buffer_health);
  void OnEndOfStreamReached();

  ErrorCB error_cb_;
  PrerolledCB prerolled_cb_;
  EndedCB ended_cb_;

  AudioStreamInfo audio_stream_info_;
  const int tunnel_mode_audio_session_id_;

  std::unique_ptr<::starboard::shared::starboard::player::filter::AudioDecoder> audio_decoder_;
  std::unique_ptr<AudioTrackWrapper> audio_track_;

  bool first_input_written_ = false;
  bool first_decoded_audio_written_ = false;
  bool end_of_stream_written_ = false;

  bool is_paused_ = true;
  double playback_rate_ = 1.0;
  int64_t seeking_to_time_ = 0;  // microseconds

  std::atomic_bool is_seeking_{false};
  std::atomic_bool end_of_stream_reached_ {false};
  std::atomic_bool has_error_{false};

#if TUNNEL_ENABLE_STATE_LOGGING
  int64_t seeking_start_at_;  // microseconds
#endif                        // TUNNEL_ENABLE_STATE_LOGGING
};

}  // namespace starboard::android::shared

#endif  // STARBOARD_ANDROID_SHARED_AUDIO_RENDERER_TUNNEL_H_