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

#include "starboard/android/shared/audio_renderer_tunnel.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "starboard/android/shared/audio_track_bridge.h"
#include "starboard/common/check_op.h"
#include "starboard/common/string.h"
#include "starboard/common/time.h"
#include "starboard/shared/starboard/media/media_util.h"
#include "starboard/shared/starboard/thread_checker.h"

namespace starboard::android::shared {

namespace {

using std::placeholders::_1;
using std::placeholders::_2;
using android::shared::AudioTrackBridge;
using ::starboard::shared::starboard::player::JobThread;
using ::starboard::shared::starboard::ThreadChecker;
using ::starboard::shared::starboard::media::GetBytesPerSample;
using ::starboard::shared::starboard::player::DecodedAudio;

//TODO: consider to send output BtyeBuffer from MediaCodec to AudioTrack directly
class AudioTrackWrapper {
public:
  AudioTrackWrapper(const ErrorCB& error_cb,
    SbMediaAudioCodingType coding_type,
    SbMediaAudioSampleType sample_type,
    int channels,
    int samples_per_second,
    int tunnel_mode_audio_session_id)
    :samples_per_second_(samples_per_second),
     bytes_per_frame_(GetBytesPerSample(sample_type)*channels),
     error_cb_(error_cb),
     job_thread_(new JobThread("audio_track_job_thread", 0, kSbThreadPriorityHigh)) {

     audio_track_bridge_ = std::make_unique<AudioTrackBridge>(coding_type,
         sample_type,
         channels, samples_per_second,
         0 /* preferred_buffer_size_in_bytes */,
        tunnel_mode_audio_session_id, false /* is_web_audio */);
     if (!audio_track_bridge_->is_valid()) {
      audio_track_bridge_.reset();
      ReportError(kSbPlayerErrorDecode, "Failed to create AudioTrackBridge.");
      return;
    }
  }

  ~AudioTrackWrapper() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());

    Flush();
    job_thread_.reset();
    audio_track_bridge_.reset();
  }

  // Data opertaions on |job_thread_|.
  void WriteSample(const scoped_refptr<DecodedAudio>& decoded_audio) {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    SB_DCHECK(!is_eos_written_);

    {
      std::lock_guard scoped_lock(mutex_);
      decoded_audios_.push_back(decoded_audio);
    }
    job_thread_->Schedule(
        std::bind(&AudioTrackWrapper::TryStartProcessInputJob, this));
  }

  void WriteEndOfStream() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    SB_DCHECK(!is_eos_written_);

    is_eos_written_ = true;
    job_thread_->Schedule(
        std::bind(&AudioTrackWrapper::TryStartProcessInputJob, this));
  }

  void Flush() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());

    is_flushing_ = true;
    job_thread_->ScheduleAndWait(
        std::bind(&AudioTrackWrapper::DoFlush, this));
    is_flushing_ = false;
  }

  // Control operations can be on player worker thread.
  void Play() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    audio_track_bridge_->Play();
  }

  void Pause() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    audio_track_bridge_->Pause();
  }

  int64_t GetAudioTimestamp(int64_t* updated_at) {
    SB_DCHECK(thread_checker_.CalledOnValidThread());

    return audio_track_bridge_->GetAudioTimestamp(updated_at);
  }

  void SetVolume(double volume) {
    SB_DCHECK(thread_checker_.CalledOnValidThread());

    audio_track_bridge_->SetVolume(volume);
  }

  void SetPlaybackRate(double playback_rate) {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    //TODO: add SetPlaybackRate
    // audio_track_bridge_->SetPlaybackRate(playback_rate);
  }

private:
  void DoProcessInput() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());
    process_input_job_token_.ResetToInvalid();

    if(has_error_ || is_flushing_) {
      return;
    }

    // TODO: Move it out of writing job.
    if (audio_track_bridge_->GetAndResetHasAudioDeviceChanged()) {
      SB_LOG(INFO) << "Audio device changed, raising a capability changed error "
                      "to restart playback.";
      ReportError(kSbPlayerErrorCapabilityChanged,
                "Audio device capability changed");
      return;
    }

    if(!decoded_audio_writing_in_progress_) {
      std::lock_guard scoped_lock(mutex_);
      if(decoded_audios_.empty()) {
        return;
      }
      decoded_audio_writing_in_progress_ = decoded_audios_.front();
      decoded_audios_.pop_front();
      decoded_audio_writing_offset_ = 0;
    }

    auto sample_buffer = decoded_audio_writing_in_progress_->data() +
                         decoded_audio_writing_offset_;
    auto samples_to_write =
        (decoded_audio_writing_in_progress_->size_in_bytes() -
         decoded_audio_writing_offset_);
    auto written_frames = decoded_audio_writing_offset_ / bytes_per_frame_;
    auto sync_time = decoded_audio_writing_in_progress_->timestamp() + ::starboard::shared::starboard::media::AudioFramesToDuration(written_frames, samples_per_second_);

    //TODO: try blocking write, to avoid parital writing.
    int samples_written = audio_track_bridge_->WriteSample(
          sample_buffer, samples_to_write, sync_time);

    if (samples_written < 0) {
      if (samples_written == AudioTrackBridge::kAudioTrackErrorDeadObject) {
        // Inform the audio end point change.
        SB_LOG(INFO)
            << "Write error for dead audio track, audio device capability "
               "has likely changed. Restarting playback.";
        ReportError(kSbPlayerErrorCapabilityChanged,
                  "Audio device capability changed");
      } else {
        // `kSbPlayerErrorDecode` is used for general SbPlayer error, there is
        // no error code corresponding to audio sink.
        ReportError(
            kSbPlayerErrorDecode,
            FormatString("Error while writing frames: %d", samples_written));
        SB_LOG(INFO) << "Encountered kSbPlayerErrorDecode while writing "
                        "frames, error: "
                     << samples_written;
      }
      return;
    }

    decoded_audio_writing_offset_ += samples_written;
    if(decoded_audio_writing_offset_ >=
          decoded_audio_writing_in_progress_->size_in_bytes()) {
      decoded_audio_writing_in_progress_ = nullptr;
      decoded_audio_writing_offset_ = 0;
    }

    TryStartProcessInputJob();
  }

  void DoFlush() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

    {
      std::lock_guard scoped_lock(mutex_);
      decoded_audios_.clear();
    }
    is_eos_written_ = false;
    decoded_audio_writing_in_progress_ = nullptr;
    decoded_audio_writing_offset_ = 0;
    audio_track_bridge_->PauseAndFlush();
  }

  void ReportError(SbPlayerError error_status,
                   const std::string& error_message) {
    if(!has_error_.exchange(true)) {
      error_cb_(error_status, error_message);
    }
  }


  void TryStartProcessInputJob() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

    if (process_input_job_token_.is_valid()) {
      // There's already an enqueued process input job.
      return;
    }
    if(is_flushing_ || has_error_) {
      return;
    }
    process_input_job_token_ = job_thread_->Schedule(
        std::bind(&AudioTrackWrapper::DoProcessInput, this));
  }

  const int samples_per_second_;
  const int bytes_per_frame_;
  const ErrorCB error_cb_;
  ThreadChecker thread_checker_;

  std::atomic_bool has_error_{false};
  std::atomic_bool is_flushing_{false};
  std::atomic_bool is_eos_written_{false};

  std::mutex mutex_;
  std::deque<scoped_refptr<DecodedAudio>> decoded_audios_;

  JobQueue::JobToken process_input_job_token_;
  scoped_refptr<DecodedAudio> decoded_audio_writing_in_progress_;
  int decoded_audio_writing_offset_ = 0;
  std::unique_ptr<AudioTrackBridge> audio_track_bridge_;
  std::unique_ptr<JobThread> job_thread_;
};

}  // namespace

AudioRendererTunnel::AudioRendererTunnel(
    std::unique_ptr<AudioDecoder> decoder,
    const AudioStreamInfo& audio_stream_info,
    int tunnel_mode_audio_session_id)
    : audio_decoder_(std::move(decoder)),
      audio_stream_info_(audio_stream_info),
      tunnel_mode_audio_session_id_(tunnel_mode_audio_session_id)  {
  SB_DLOG(INFO) << "Creating AudioRendererTunnel with " << audio_stream_info_;
  SB_DCHECK(audio_decoder_);
}

AudioRendererTunnel::~AudioRendererTunnel() {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DLOG(INFO) << "Destroying AudioRendererTunnel with " << audio_stream_info_;

}

void AudioRendererTunnel::Initialize(const ErrorCB& error_cb,
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

  // audio_decoder_->Initialize(std::bind(&AudioRendererTunnel::OnDecoderOutput, this),
  //                      std::bind(&AudioRendererTunnel::ReportError, this, _1, _2));

  // TODO: initialize AudioSink
  // TODO: verify if output format could change, if so, re-create the sink when necessary
}

void AudioRendererTunnel::WriteSamples(const InputBuffers& input_buffers) {
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

  //TODO: add audio frame tracker
  //TODO: write samples
}

void AudioRendererTunnel::WriteEndOfStream() {
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

  //TODO: write eos
}

void AudioRendererTunnel::Seek(int64_t seek_to_time) {

}

void AudioRendererTunnel::SetVolume(double volume) {
  SB_DCHECK(BelongsToCurrentThread());

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }

  //TODO: set volume
}

bool AudioRendererTunnel::IsEndOfStreamWritten() const {
  SB_DCHECK(BelongsToCurrentThread());
  return end_of_stream_written_;
}

bool AudioRendererTunnel::IsEndOfStreamPlayed() const {
  SB_DCHECK(BelongsToCurrentThread());
  //TODO: make it right
  return end_of_stream_written_;
}

bool AudioRendererTunnel::CanAcceptMoreData() const {
  SB_DCHECK(BelongsToCurrentThread());
  //TODO: make it right
  return true;
}



}  // starboard::android::shared