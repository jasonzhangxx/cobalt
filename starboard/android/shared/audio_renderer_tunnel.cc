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

#include "starboard/android/shared/audio_track_audio_sink_type.h"
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
using ::starboard::shared::starboard::media::AudioFramesToDuration;
using ::starboard::shared::starboard::media::GetBytesPerSample;
using ::starboard::shared::starboard::player::DecodedAudio;


class PerformanceTimer {
public:
  PerformanceTimer(const std::string& name)
    : name_(name),
      started_at_(CurrentMonotonicTime()) {
  }
  ~PerformanceTimer() {
    SB_DLOG(INFO)<<name_<<" takes "<< CurrentMonotonicTime() - started_at_
                 << " microseconds.";
  }
private:
  std::string name_;
  int64_t started_at_;
};

}  // namespace

//TODO: consider to send output BtyeBuffer from MediaCodec to AudioTrack directly
class AudioTrackWrapper {
public:
  // enum class BufferHealth {
  //   kUnderrun,  // Buffered data is not enough to keep playing.
  //   kLow, //  buffered data < low_watermark
  //   kHealthy, // low_watermark <= buffered data <= high_watermark
  //   kFull,  // buffered data > high_watermark
  // };

  using OnBufferHealthChangedCallback = std::function<void(BufferHealth)>;
  using OnEndOfStreamReachedCallback = std::function<void()>;

  AudioTrackWrapper(
    const OnBufferHealthChangedCallback& buffer_health_changed_cb,
    const OnEndOfStreamReachedCallback& eos_reached_cb,
    const ErrorCB& error_cb,
    SbMediaAudioCodingType coding_type,
    SbMediaAudioSampleType sample_type,
    int channels,
    int samples_per_second,
    int tunnel_mode_audio_session_id)
    : buffer_health_changed_cb_(buffer_health_changed_cb),
     eos_reached_cb_(eos_reached_cb),
     error_cb_(error_cb),
     sample_type_(sample_type),
     channels_(channels),
     samples_per_second_(samples_per_second),
     bytes_per_frame_(GetBytesPerSample(sample_type)*channels),
     // TODO: tune the watermark values
     frames_underflow_watermark_(1024),
     frames_low_watermark_(AudioTrackAudioSinkType::GetMinBufferSizeInFrames(channels,
        sample_type, samples_per_second)),
     frames_high_watermark_(samples_per_second /* 1s of audio */),
     frames_max_watermark_(samples_per_second * 10 /* 10s of audio */),
     job_thread_(new JobThread("audio_track_job_thread", 0, kSbThreadPriorityHigh)) {

    SB_DCHECK(buffer_health_changed_cb_);
    SB_DCHECK(error_cb_);

     SB_DLOG(INFO)<<"Creating AudioTrackWrapper with low watermark "<<frames_low_watermark_;

     // Ask for buffer size larger than |frames_low_watermark_| to ensure enough buffer room for prerolling. And set the size larger than 2x of min buffer size to support up to 2x playback speed.
     int preferred_buffer_size_in_frames = std::max(frames_low_watermark_, AudioTrackBridge::GetMinBufferSizeInFrames(sample_type, channels, samples_per_second) * 2);
     audio_track_bridge_ = std::make_unique<AudioTrackBridge>(coding_type,
         sample_type,
         channels, samples_per_second,
         preferred_buffer_size_in_frames * bytes_per_frame_,
         tunnel_mode_audio_session_id, false /* is_web_audio */);

    if (!audio_track_bridge_->is_valid()) {
      audio_track_bridge_.reset();
      ReportError(kSbPlayerErrorDecode,
        FormatString("Failed to create AudioTrackBridge with coding type %d, sample type %d, channels %d, samples per second %d, session id %d.",
          coding_type, sample_type, channels, samples_per_second, tunnel_mode_audio_session_id));
    }
  }

  ~AudioTrackWrapper() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    // |job_thread_| will be set to nullptr before deleting the underlying object and |job_thread_| is used
    // in some jobs, so we need to ensure all jobs are flushed before reset it.
    Flush();
    job_thread_.reset();
  }

  bool IsPaused() const {
    return is_paused_;
  }

  // Data opertaions on |job_thread_|.
  void WriteDecodedAudio(const scoped_refptr<DecodedAudio>& decoded_audio) {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    SB_DCHECK(!end_of_stream_written_);
    {
      std::lock_guard scoped_lock(mutex_);
      decoded_audios_.push_back(decoded_audio);
    }
    total_input_frames_ += decoded_audio->frames();
    job_thread_->Schedule(
        std::bind(&AudioTrackWrapper::TryStartProcessInputJob, this, 0 /* delay */));
  }

  void WriteEndOfStream() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    end_of_stream_written_ = true;
    job_thread_->Schedule(
        std::bind(&AudioTrackWrapper::TryStartProcessInputJob, this, 0 /* delay */));
  }

  void Flush() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());

    is_flushing_ = true;
    job_thread_->ScheduleAndWait(
        std::bind(&AudioTrackWrapper::DoFlush, this));
    is_flushing_ = false;
    is_paused_ = true;
  }

  // Control operations can be on player worker thread.
  void Play() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    if(has_error_ || !is_paused_) {
      return;
    }
    audio_track_bridge_->Play();
    is_paused_ = false;

    job_thread_->Schedule(std::bind(&AudioTrackWrapper::DoUpdateAudioHeadPosition, this));
  }

  void Pause() {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    if(has_error_ || is_paused_) {
      return;
    }
    audio_track_bridge_->Pause();
    is_paused_ = true;

    job_thread_->Schedule(std::bind(&AudioTrackWrapper::DoUpdateAudioHeadPosition, this));
  }

  // TODO: add a structure for return values
  int64_t GetHeadPosition(bool *is_advancing, int64_t* updated_at) {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    if(has_error_) {
      return 0;
    }

    std::lock_guard scoped_lock(mutex_);
    *is_advancing = audio_head_is_advancing_;
    *updated_at = audio_head_position_update_at_;
    return audio_head_position_;
  }

  int64_t GetEstimatedPendingFrames() const {
    // The estimated value could have a discrepancy. We try our best to make more accurate.
    return std::max(total_input_frames_ - total_played_frames_, 0);
  }

  BufferHealth GetBufferHealth() const {
    return buffer_health_;
  }

  void SetVolume(double volume) {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    if(has_error_) {
      return;
    }
    audio_track_bridge_->SetVolume(volume);
  }

  void SetPlaybackRate(double playback_rate) {
    SB_DCHECK(thread_checker_.CalledOnValidThread());
    if(has_error_) {
      return;
    }
    audio_track_bridge_->SetPlaybackRate(playback_rate);
  }

private:
  void DoProcessInput() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());
    process_input_job_token_.ResetToInvalid();

    if(has_error_ || is_flushing_) {
      return;
    }

    // Write next decoded audio if there's one.
    if(!decoded_audio_writing_in_progress_) {
      std::lock_guard scoped_lock(mutex_);
      if(!decoded_audios_.empty()) {
        decoded_audio_writing_in_progress_ = decoded_audios_.front();
        decoded_audios_.pop_front();
        decoded_audio_writing_offset_ = 0;
      }
    }
    // Write silence audio after eos.
    if(!decoded_audio_writing_in_progress_ && end_of_stream_written_) {
      if(!silenced_decoded_audio_) {
        InitializeSilencedDecodedAudio();
      }
      int64_t timestamp = AudioFramesToDuration(total_written_frames_, samples_per_second_);
      decoded_audio_writing_in_progress_ = silenced_decoded_audio_;
      decoded_audio_writing_offset_ = 0;
    }
    if(!decoded_audio_writing_in_progress_) {
      // No more decoded audio to write.
      return;
    }

    auto sample_buffer = decoded_audio_writing_in_progress_->data() +
                         decoded_audio_writing_offset_;
    auto samples_to_write =
        (decoded_audio_writing_in_progress_->size_in_bytes() -
         decoded_audio_writing_offset_);
    auto written_frames = decoded_audio_writing_offset_ / bytes_per_frame_;
    auto sync_time = decoded_audio_writing_in_progress_->timestamp() + AudioFramesToDuration(written_frames, samples_per_second_);

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

    total_written_frames_ += samples_written;
    decoded_audio_writing_offset_ += samples_written;
    if(decoded_audio_writing_offset_ >=
          decoded_audio_writing_in_progress_->size_in_bytes()) {
      decoded_audio_writing_in_progress_ = nullptr;
      decoded_audio_writing_offset_ = 0;
    }

    if(samples_written > 0) {
      UpdateBufferHealth();
    }

    BufferHealth buffer_health = GetBufferHealth();
    // TODO: tune delays
    if(samples_written == 0 || decoded_audio_writing_in_progress_) {
      // AudioTrack buffer is full.
      if(buffer_health >= BufferHealth::kFull) {
        TryStartProcessInputJob(20'000);
        SB_DLOG(INFO)<<"AudioTrack's buffer is full and watermark is full. Schedule next writing with delay of 20ms";
      }
      else if(buffer_health == BufferHealth::kHealthy) {
        TryStartProcessInputJob(10'000);
        SB_DLOG(INFO)<<"AudioTrack's buffer is full and watermark is health. Schedule next writing with delay of 10ms";
      }
      else {
        if(!is_paused_) {
          TryStartProcessInputJob(10'000);
          SB_DLOG(INFO)<<"AudioTrack's buffer is full but watermark is low. Schedule next writing with delay of 10ms";
        }
        else {
          TryStartProcessInputJob(20'000);
          SB_DLOG(INFO)<<"AudioTrack's buffer is full and audio is not playing. Schedule next writing with delay of 20ms";
        }
      }
    }
    else  {
      // AudioTrack buffer is not full.
      if(buffer_health >= BufferHealth::kFull) {
        TryStartProcessInputJob(10'000);
        SB_DLOG(INFO)<<"AudioTrack's buffer is not full but watermark is full. Schedule next writing with delay of 10ms";
      }
      else {
        TryStartProcessInputJob();
        SB_DLOG(INFO)<<"AudioTrack's buffer is not full and watermark is low. Schedule next writing with delay of 0ms";
      }
    }
  }

  void DoUpdateAudioHeadPosition() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());
    if(update_head_position_job_token_.is_valid()) {
      job_thread_->RemoveJobByToken(update_head_position_job_token_);
      update_head_position_job_token_.ResetToInvalid();
    }

    if(has_error_ || is_flushing_) {
      return;
    }

    PerformanceTimer("DoUpdateAudioHeadPosition");

    // TODO: maybe change the function name?
    if (audio_track_bridge_->GetAndResetHasAudioDeviceChanged()) {
      SB_LOG(INFO) << "Audio device changed, raising a capability changed error "
                      "to restart playback.";
      ReportError(kSbPlayerErrorCapabilityChanged,
                "Audio device capability changed");
      return;
    }

    int64_t updated_at = 0;
    int64_t head_position = audio_track_bridge_->GetAudioTimestamp(&updated_at);
    if(head_position > 0) {
      std::lock_guard scoped_lock(mutex_);
      audio_head_is_advancing_ = audio_head_position_!=head_position;
      audio_head_position_ = head_position;
      audio_head_position_update_at_ = updated_at;
      // TODO: |total_played_frames_| and |audio_head_position_| are identical.
      total_played_frames_ = audio_head_position_;

      // TODO: tune the delay
      if(!is_paused_) {
        if(!audio_head_is_advancing_) {
          // Audio is starting.
          update_head_position_job_token_ = job_thread_->Schedule(
                  std::bind(&AudioTrackWrapper::DoUpdateAudioHeadPosition, this), 20'000);
          SB_DLOG(INFO)<<"Audio is starting. Schedule next update with delay of 20ms";
        }
        else if(end_of_stream_written_ && !end_of_stream_reached_ && AudioFramesToDuration(GetEstimatedPendingFrames(), samples_per_second_) < 200'000) {
          // Audio is ending.
          update_head_position_job_token_ = job_thread_->Schedule(
                  std::bind(&AudioTrackWrapper::DoUpdateAudioHeadPosition, this), 20'000);
          SB_DLOG(INFO)<<"Audio will end in "<<AudioFramesToDuration(GetEstimatedPendingFrames(), samples_per_second_)<<"ms. Schedule next update with delay of 20ms";
        } 
        else {
          // Audio is playing at a stable rate.
          update_head_position_job_token_ = job_thread_->Schedule(
                  std::bind(&AudioTrackWrapper::DoUpdateAudioHeadPosition, this), 200'000);
          SB_DLOG(INFO)<<"Audio is playing. Schedule next update with delay of 200ms";
        }
      }
      else {
        if(audio_head_is_advancing_) {
          // Audio is pausing.
          update_head_position_job_token_ = job_thread_->Schedule(
                  std::bind(&AudioTrackWrapper::DoUpdateAudioHeadPosition, this), 20'000);
          SB_DLOG(INFO)<<"Audio is pausing. Schedule next update with delay of 20ms";
        }
        else {
          // Audio is paused, stop updating.
          SB_DLOG(INFO)<<"Audio is paused. Stop updating";
        }
      }
    }
    else {
      // AudioTrack can't give us a valid head position.
      if(!is_paused_) {
        // Audio is warming up.
        update_head_position_job_token_ = job_thread_->Schedule(
                std::bind(&AudioTrackWrapper::DoUpdateAudioHeadPosition, this), 20'000);
        SB_DLOG(INFO)<<"Audio is warming up. Schedule next update with delay of 20ms";
      }
      else {
        // Unexpected error when audio is not playing, stop updating.
        SB_DLOG(INFO)<<"Unexpected error when audio is not playing, stop updating";
      }
    }

    // TODO: refine this code block the one above
    if(head_position > 0) {
      UpdateBufferHealth();
      if(end_of_stream_written_ && !end_of_stream_reached_ && (total_played_frames_ > total_input_frames_)) {
        end_of_stream_reached_ = true;
        eos_reached_cb_();
      }
    }
  }

  void DoFlush() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

    // Cancel pending delayed jobs.
    if(update_head_position_job_token_.is_valid()) {
      job_thread_->RemoveJobByToken(update_head_position_job_token_);
      update_head_position_job_token_.ResetToInvalid();
    }
    if(process_input_job_token_.is_valid()) {
      job_thread_->RemoveJobByToken(process_input_job_token_);
      process_input_job_token_.ResetToInvalid();
    }

    {
      std::lock_guard scoped_lock(mutex_);
      decoded_audios_.clear();
      audio_head_is_advancing_ = false;
      audio_head_position_ = 0;
      audio_head_position_update_at_ = 0;
    }
    if(audio_track_bridge_) {
      audio_track_bridge_->PauseAndFlush();
    }
    end_of_stream_written_ = false;
    end_of_stream_reached_ = false;
    decoded_audio_writing_in_progress_ = nullptr;
    decoded_audio_writing_offset_ = 0;
    total_input_frames_ = 0;
    total_written_frames_ = 0;
    total_played_frames_ = 0;
  }

  void ReportError(SbPlayerError error_status,
                   const std::string& error_message) {
    if(!has_error_.exchange(true)) {
      error_cb_(error_status, error_message);
    }
  }

  void TryStartProcessInputJob(int64_t delay = 0) {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

    if(is_flushing_ || has_error_) {
      return;
    }
    if (process_input_job_token_.is_valid()) {
      // There's already an enqueued process input job.
      return;
    }
    process_input_job_token_ = job_thread_->Schedule(
        std::bind(&AudioTrackWrapper::DoProcessInput, this), delay);
  }

  void InitializeSilencedDecodedAudio() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());
    SB_DCHECK(!silenced_decoded_audio_);

    silenced_decoded_audio_ = new DecodedAudio(channels_, sample_type_, 
      kSbMediaAudioFrameStorageTypeInterleaved, 0 /* timestamp */, 1024 * bytes_per_frame_);
    memset(silenced_decoded_audio_->data(), 0, silenced_decoded_audio_->size_in_bytes());
  }

  // Returned value includes silence frames appended after eos.
  int64_t GetEstimatedBufferedFrames() const {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());
    // The estimated value could have a discrepancy. We try our best to make more accurate.
    return total_written_frames_ - total_played_frames_;
  }

  void UpdateBufferHealth() {
    SB_DCHECK(job_thread_->BelongsToCurrentThread());

    BufferHealth new_state;
    int64_t estimated_buffered_frames = GetEstimatedBufferedFrames();
    if(estimated_buffered_frames < frames_underflow_watermark_) {
      new_state = BufferHealth::kUnderrun;
    }
    else if(estimated_buffered_frames < frames_low_watermark_) {
      new_state = BufferHealth::kLow;
    }
    else if(estimated_buffered_frames <= frames_high_watermark_) {
      new_state = BufferHealth::kHealthy;
    }
    else {
      new_state = BufferHealth::kFull;
    }

    if(buffer_health_ != new_state) {
      buffer_health_ = new_state;
      buffer_health_changed_cb_(new_state);
    }
  }

  const OnBufferHealthChangedCallback buffer_health_changed_cb_;
  const OnEndOfStreamReachedCallback eos_reached_cb_;
  const ErrorCB error_cb_;

  const SbMediaAudioSampleType sample_type_;
  const int channels_;
  const int samples_per_second_;
  const int bytes_per_frame_;

  const int frames_underflow_watermark_;
  const int frames_low_watermark_;
  const int frames_high_watermark_;
  const int frames_max_watermark_;

  ThreadChecker thread_checker_;
  std::atomic_bool is_paused_ {true};
  std::atomic_bool has_error_{false};
  std::atomic_bool is_flushing_{false};
  std::atomic_bool end_of_stream_written_ {false};
  std::atomic_bool end_of_stream_reached_ {false};

  std::atomic_int total_input_frames_ {0};
  std::atomic_int total_written_frames_ {0};
  std::atomic_int total_played_frames_ {0};
  std::atomic<BufferHealth> buffer_health_ {BufferHealth::kUnderrun};

  scoped_refptr<DecodedAudio> silenced_decoded_audio_;

  std::mutex mutex_;
  std::deque<scoped_refptr<DecodedAudio>> decoded_audios_;
  bool audio_head_is_advancing_ = false;
  int64_t audio_head_position_ = 0;
  int64_t audio_head_position_update_at_ = 0;

  JobQueue::JobToken update_head_position_job_token_;
  JobQueue::JobToken process_input_job_token_;
  scoped_refptr<DecodedAudio> decoded_audio_writing_in_progress_;
  int decoded_audio_writing_offset_ = 0;

  std::unique_ptr<AudioTrackBridge> audio_track_bridge_;
  std::unique_ptr<JobThread> job_thread_;
};

AudioRendererTunneled::AudioRendererTunneled(
    const AudioStreamInfo& audio_stream_info,
    std::unique_ptr<::starboard::shared::starboard::player::filter::AudioDecoder> decoder,
    int tunnel_mode_audio_session_id)
    : audio_stream_info_(audio_stream_info),
      audio_decoder_(std::move(decoder)),
      tunnel_mode_audio_session_id_(tunnel_mode_audio_session_id)  {
  SB_DLOG(INFO) << "Creating AudioRendererTunneled with " << audio_stream_info_;
}

AudioRendererTunneled::~AudioRendererTunneled() {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DLOG(INFO) << "Destroying AudioRendererTunneled with " << audio_stream_info_;

  audio_decoder_->Reset();
  TeardownAudioTrack();
}

void AudioRendererTunneled::Initialize(const ErrorCB& error_cb,
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

  audio_decoder_->Initialize(std::bind(&AudioRendererTunneled::OnDecoderOutput, this),
                       std::bind(&AudioRendererTunneled::ReportError, this, _1, _2));
  InitializeAudioTrack();
}

void AudioRendererTunneled::WriteSamples(const InputBuffers& input_buffers) {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(!input_buffers.empty());
  for (const auto& input_buffer : input_buffers) {
    SB_DCHECK(input_buffer);
  }

  SB_DLOG(INFO)<<"WriteSamples";

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
      //TODO: check if it needs to re-create decoder
  }

  audio_decoder_->Decode(input_buffers,
                   std::bind(&AudioRendererTunneled::OnDecoderConsumed, this));
}

void AudioRendererTunneled::WriteEndOfStream() {
  SB_DCHECK(BelongsToCurrentThread());

  SB_DLOG(INFO)<<"WriteSamples";

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

  audio_decoder_->WriteEndOfStream();
}

void AudioRendererTunneled::SetVolume(double volume) {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(audio_track_);

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }
  audio_track_->SetVolume(volume);
}

bool AudioRendererTunneled::IsEndOfStreamWritten() const {
  SB_DCHECK(BelongsToCurrentThread());
  return end_of_stream_written_;
}

bool AudioRendererTunneled::IsEndOfStreamPlayed() const {
  SB_DCHECK(BelongsToCurrentThread());
  return end_of_stream_reached_;
}

bool AudioRendererTunneled::CanAcceptMoreData() const {
  SB_DCHECK(BelongsToCurrentThread());
  if (has_error_) {
    // Ignore the request when there's an error.
    return false;
  }
  // TODO: tune the value
  return AudioFramesToDuration(audio_track_->GetEstimatedPendingFrames(), audio_stream_info_.samples_per_second) < 10'000'000;
}

void AudioRendererTunneled::Play() {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(audio_track_);

  SB_DLOG(INFO)<<"Play";

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }

  PerformanceTimer("AudioRendererTunneled::Play");
  is_paused_ = false;
  UpdateAudioTrackPlayingState();
}

void AudioRendererTunneled::Pause() {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(audio_track_);

  SB_DLOG(INFO)<<"Pause";

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }

  PerformanceTimer("AudioRendererTunneled::Pause");
  is_paused_ = true;
  UpdateAudioTrackPlayingState();
}

void AudioRendererTunneled::SetPlaybackRate(double playback_rate) {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(playback_rate >= 0);
  SB_DCHECK(audio_track_);

  SB_DLOG(INFO)<<"SetPlaybackRate "<<playback_rate;

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }

  PerformanceTimer("AudioRendererTunneled::SetPlaybackRate");
  playback_rate_ = playback_rate;
  // Android AudioTrack doesn't support playback rate of 0.
  if(playback_rate_ > 0.0) {
    audio_track_->SetPlaybackRate(playback_rate_);
  }
  UpdateAudioTrackPlayingState();
}

void AudioRendererTunneled::Seek(int64_t seek_to_time) {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(audio_track_);
  SB_DCHECK_GE(seek_to_time, 0);

  SB_DLOG(INFO)<<"Seek to "<<seek_to_time;

  if (has_error_) {
    // Ignore the request when there's an error.
    return;
  }

#if TUNNEL_ENABLE_STATE_LOGGING
  SB_LOG(INFO) << "Start seeking.";
  seeking_start_at_ = CurrentMonotonicTime();
#endif  // TUNNEL_ENABLE_STATE_LOGGING

  PerformanceTimer("AudioRendererTunneled::Seek");

  if (first_input_written_) {
    audio_decoder_->Reset();
    if(first_decoded_audio_written_) {
      audio_track_->Flush();
    }
    first_input_written_ = false;
    first_decoded_audio_written_ = false;
    end_of_stream_written_ = false;
    end_of_stream_reached_ = false;
  }

  is_seeking_ = true;
  is_paused_ = true;
  seeking_to_time_ = seek_to_time;
}

int64_t AudioRendererTunneled::GetCurrentMediaTime(bool* is_playing,
                            bool* is_eos_played,
                            bool* is_underflow,
                            double* playback_rate) {
  SB_DCHECK(BelongsToCurrentThread());
  SB_DCHECK(audio_track_);

  bool is_advancing = false;
  int64_t updated_at = 0;
  int64_t playback_head_position = audio_track_->GetHeadPosition(&is_advancing, &updated_at);

  int64_t audio_timestamp = seeking_to_time_;
  audio_timestamp += AudioFramesToDuration(playback_head_position, audio_stream_info_.samples_per_second);
  if(is_advancing) {
    audio_timestamp += CurrentMonotonicTime() - updated_at;
  }

  *is_playing = is_advancing;
  *is_eos_played = IsEndOfStreamPlayed();
  //TODO: handle underflow
  *is_underflow = false;
  *playback_rate = playback_rate_;

  SB_DLOG(INFO) << "GetCurrentMediaTime is_playing: "<<*is_playing
    <<", is_eos_played: "<<*is_eos_played
    <<", playback_rate: "<<*playback_rate
    <<", audio_timestamp: "<<audio_timestamp;
  return audio_timestamp;
}

void AudioRendererTunneled::InitializeAudioTrack() {
  SB_DCHECK(!audio_track_);

  SB_DLOG(INFO)<<"InitializeAudioTrack";
  PerformanceTimer("AudioRendererTunneled::InitializeAudioTrack");

  // Currently tunnel mode only supports int16 pcm.
  audio_track_.reset(new AudioTrackWrapper(
    std::bind(&AudioRendererTunneled::OnBufferHealthChanged, this, _1),
    std::bind(&AudioRendererTunneled::OnEndOfStreamReached, this),
    std::bind(&AudioRendererTunneled::ReportError, this, _1, _2),
    kSbMediaAudioCodingTypePcm,
    kSbMediaAudioSampleTypeInt16Deprecated,
    audio_stream_info_.number_of_channels,
    audio_stream_info_.samples_per_second,
    tunnel_mode_audio_session_id_));
}

void AudioRendererTunneled::TeardownAudioTrack() {
  SB_DLOG(INFO)<<"TeardownAudioTrack";
  PerformanceTimer("AudioRendererTunneled::TeardownAudioTrack");

  // TODO: need to stop audio track first to prevent invoking callbacks
  audio_track_.reset();
}

void AudioRendererTunneled::TryToSignalPreroll() {
  if (is_seeking_.exchange(false)) {
#if TUNNEL_ENABLE_STATE_LOGGING
    SB_LOG(INFO) << "Audio preroll takes "
                 << CurrentMonotonicTime() - seeking_start_at_
                 << " microseconds.";
#endif  // TUNNEL_ENABLE_STATE_LOGGING
    prerolled_cb_();
  }
}

void AudioRendererTunneled::UpdateAudioTrackPlayingState() {
  if(!is_paused_ && playback_rate_ > 0) {
    audio_track_->Play();
  }
  else {
    audio_track_->Pause();
  }
}

void AudioRendererTunneled::ReportError(const SbPlayerError error,
                                      const std::string error_message) {
  SB_DCHECK(error_cb_);
  if (!has_error_.exchange(true)) {
    SB_LOG(ERROR) << "Unrecoverable error (audio): " << error_message;
    error_cb_(error, error_message);
  }
}

void AudioRendererTunneled::OnDecoderConsumed() {
  SB_DCHECK(BelongsToCurrentThread());

}

void AudioRendererTunneled::OnDecoderOutput() {
  SB_DCHECK(BelongsToCurrentThread());

  int decoded_audio_sample_rate;
  scoped_refptr<DecodedAudio> decoded_audio = audio_decoder_->Read(&decoded_audio_sample_rate);
  

  if(decoded_audio->is_end_of_stream()) {
    audio_track_->WriteEndOfStream();
    first_decoded_audio_written_ = true;
  }
  else {
    if(!first_decoded_audio_written_) {
      //TODO: check if it needs to re-create AudioTrack.
      first_decoded_audio_written_ = true;
    }
    if(decoded_audio->timestamp() < seeking_to_time_) {
      decoded_audio->AdjustForSeekTime(audio_stream_info_.samples_per_second, seeking_to_time_);
    }
    audio_track_->WriteDecodedAudio(decoded_audio);
  }
}

void AudioRendererTunneled::OnBufferHealthChanged(BufferHealth buffer_health) {
  SB_DLOG(INFO)<<"AudioRendererTunneled OnBufferHealthChanged "<< (int)buffer_health;
  // It's called on AudioTrack thread.
  if(is_seeking_ && buffer_health >= BufferHealth::kHealthy) {
    TryToSignalPreroll();
  }
  if(buffer_health == BufferHealth::kUnderrun && !audio_track_->IsPaused()) {
    // TODO: process underrun
  }
}

void AudioRendererTunneled::OnEndOfStreamReached() {
  SB_DLOG(INFO)<<"AudioRendererTunneled OnEndOfStreamReached";
  // It should only happen once until flushed.
  SB_DCHECK(!end_of_stream_reached_);
  end_of_stream_reached_ = true;
  ended_cb_();
}

}  // starboard::android::shared