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

#ifndef STARBOARD_ANDROID_SHARED_TUNNEL_RENDERER_SYNCHRONIZER_H_
#define STARBOARD_ANDROID_SHARED_TUNNEL_RENDERER_SYNCHRONIZER_H_

#include "starboard/common/log.h"
#include "starboard/media.h"
#include "starboard/shared/internal_only.h"
#include "starboard/shared/starboard/player/filter/media_time_provider.h"

namespace starboard::android::shared {

using ::starboard::shared::starboard::player::filter::MediaTimeProvider;

class TunnelRendererSynchronizer : public MediaTimeProvider {
 public:
  TunnelRendererSynchronizer();
  ~TunnelRendererSynchronizer() override;

  void Play() override;
  void Pause() override;
  void SetPlaybackRate(double playback_rate) override;
  void Seek(int64_t seek_to_time) override;
  int64_t GetCurrentMediaTime(bool* is_playing,
                              bool* is_eos_played,
                              bool* is_underflow,
                              double* playback_rate) override;

 private:

};

}  // starboard::android::shared

#endif  // STARBOARD_ANDROID_SHARED_TUNNEL_RENDERER_SYNCHRONIZER_H_