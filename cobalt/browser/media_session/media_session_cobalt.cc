// Copyright 2025 The Cobalt Authors. All Rights Reserved.
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

#include "cobalt/browser/media_session/media_session_cobalt.h"

#include <codecvt>

#include "cobalt/media_session/media_image.h"
#include "content/browser/media/session/media_session_impl.h"
#include "content/browser/media/session/media_session_player_observer.h"
#include "starboard/system.h"

namespace cobalt {
namespace media_session {
namespace {
std::string u16string_to_string(const std::u16string& u16str) {
  std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
  std::string utf8_str = convert.to_bytes(u16str);
  return utf8_str;
}

// A class help to build CobaltExtensionMediaSessionState and hold actual data.
class CobaltExtensionMediaSessionStateHolder {
 public:
  CobaltExtensionMediaSessionState GetSessionState() {}

 private:
  CobaltExtensionMediaSessionState session_state_;
  CobaltExtensionMediaMetadata metadata_;

  std::string title;
  std::string artist;
  std::string album;
  std::vector<CobaltExtensionMediaImage> artworks_;
};
}  // namespace

MediaSessionCobalt::MediaSessionCobalt(content::MediaSessionImpl* session)
    : media_session_(session) {
  DCHECK(media_session_);

  session->AddObserver(observer_receiver_.BindNewPipeAndPassRemote());

  extension_ = static_cast<const CobaltExtensionMediaSessionApi*>(
      SbSystemGetExtension(kCobaltExtensionMediaSessionName));
  if (extension_) {
    if (strcmp(extension_->name, kCobaltExtensionMediaSessionName) != 0 ||
        extension_->version < 1) {
      LOG(WARNING) << "Wrong MediaSession extension supplied";
      extension_ = nullptr;
    } else if (extension_->RegisterMediaSessionCallbacks != nullptr) {
      extension_->RegisterMediaSessionCallbacks(
          this, &InvokeActionCallback, &UpdatePlatformPlaybackStateCallback);
      DCHECK(extension_->DestroyMediaSessionClientCallback)
          << "Possible heap use-after-free if platform does not handle media "
          << "session DestroyMediaSessionClientCallback()";
    }
  }
}

MediaSessionCobalt::~MediaSessionCobalt() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

void MediaSessionCobalt::MediaSessionInfoChanged(
    ::media_session::mojom::MediaSessionInfoPtr session_info) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  LOG(ERROR) << "JA: MediaSessionInfoChanged";

  bool is_paused = session_info->playback_state ==
                   ::media_session::mojom::MediaPlaybackState::kPaused;
  if (is_paused_ == is_paused) {
    return;
  }
  is_paused_ = is_paused;
  UpdateMediaSessionState();
}

void MediaSessionCobalt::MediaSessionMetadataChanged(
    const absl::optional<::media_session::MediaMetadata>& metadata) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  LOG(ERROR) << "JA: MediaSessionMetadataChanged";

  if (metadata_ == metadata) {
    return;
  }
  metadata_ = metadata;
  UpdateMediaSessionState();
}

void MediaSessionCobalt::MediaSessionActionsChanged(
    const std::vector<::media_session::mojom::MediaSessionAction>& actions) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  LOG(ERROR) << "JA: MediaSessionActionsChanged";

  if (actions_ == actions) {
    return;
  }
  actions_ = actions;
  UpdateMediaSessionState();
}

void MediaSessionCobalt::MediaSessionImagesChanged(
    const base::flat_map<::media_session::mojom::MediaSessionImageType,
                         std::vector<::media_session::MediaImage>>& images) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  LOG(ERROR) << "JA: MediaSessionImagesChanged";

  auto it =
      images.find(::media_session::mojom::MediaSessionImageType::kArtwork);
  if (artworks_ == it->second) {
    return;
  }

  artworks_ = it->second;
  UpdateMediaSessionState();
}

void MediaSessionCobalt::MediaSessionPositionChanged(
    const absl::optional<::media_session::MediaPosition>& position) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  LOG(ERROR) << "JA: MediaSessionPositionChanged";

  if (position_ == position) {
    return;
  }
  position_ = position;
  UpdateMediaSessionState();
}

// static
void MediaSessionCobalt::InvokeActionCallback(
    CobaltExtensionMediaSessionActionDetails details,
    void* callback_context) {
  DCHECK(callback_context);

  MediaSessionCobalt* client =
      static_cast<MediaSessionCobalt*>(callback_context);
  client->InvokeAction(details);
}

// static
void MediaSessionCobalt::UpdatePlatformPlaybackStateCallback(
    CobaltExtensionMediaSessionPlaybackState state,
    void* callback_context) {
  DCHECK(callback_context);

  MediaSessionCobalt* client =
      static_cast<MediaSessionCobalt*>(callback_context);
  client->UpdatePlatformPlaybackState(state);
}

void MediaSessionCobalt::InvokeAction(
    const CobaltExtensionMediaSessionActionDetails& details) {
  LOG(ERROR) << "JA: InvokeAction";
}

void MediaSessionCobalt::UpdatePlatformPlaybackState(
    CobaltExtensionMediaSessionPlaybackState state) {
  LOG(ERROR) << "JA: UpdatePlatformPlaybackState";
}

void MediaSessionCobalt::UpdateMediaSessionState() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (!extension_) {
    return;
  }

  CobaltExtensionMediaSessionState session_state;

  if (metadata_ && !metadata_.IsEmpty()) {
    title = u16string_to_string(metadata_.title);
    artist = u16string_to_string(metadata_.artist);
    album = u16string_to_string(metadata_.album);
  }

  if (!actions_.empty()) {
  }

  if (!artworks_.empty()) {
  }

  if (position_) {
  }

  extension_->OnMediaSessionStateChanged(session_state);
}

}  // namespace media_session
}  // namespace cobalt
