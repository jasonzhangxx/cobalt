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

#ifndef COBALT_BROWSER_MEDIA_SESSION_MEDIA_SESSION_COBALT_H_
#define COBALT_BROWSER_MEDIA_SESSION_MEDIA_SESSION_COBALT_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/threading/thread_checker.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "starboard/extension/media_session.h"

namespace content {
class MediaSessionImpl;
}  // namespace content

namespace cobalt {
namespace media_session {

// This class is interlayer between native MediaSession and Cobalt
// MediaSession extension.
class MediaSessionCobalt final
    : public ::media_session::mojom::MediaSessionObserver {
 public:
  explicit MediaSessionCobalt(content::MediaSessionImpl* session);
  ~MediaSessionCobalt() override;

  MediaSessionCobalt(const MediaSessionCobalt&) = delete;
  MediaSessionCobalt& operator=(const MediaSessionCobalt&) = delete;

  // media_session::mojom::MediaSessionObserver implementation:
  void MediaSessionInfoChanged(
      ::media_session::mojom::MediaSessionInfoPtr session_info) override;
  void MediaSessionMetadataChanged(
      const absl::optional<::media_session::MediaMetadata>& metadata) override;
  void MediaSessionActionsChanged(
      const std::vector<::media_session::mojom::MediaSessionAction>& actions)
      override;
  void MediaSessionImagesChanged(
      const base::flat_map<::media_session::mojom::MediaSessionImageType,
                           std::vector<::media_session::MediaImage>>& images)
      override;
  void MediaSessionPositionChanged(
      const absl::optional<::media_session::MediaPosition>& position) override;

 private:
  // Static callback wrappers for MediaSessionAPI extension.
  static void InvokeActionCallback(
      CobaltExtensionMediaSessionActionDetails details,
      void* callback_context);
  static void UpdatePlatformPlaybackStateCallback(
      CobaltExtensionMediaSessionPlaybackState state,
      void* callback_context);

  void InvokeAction(const CobaltExtensionMediaSessionActionDetails& details);
  void UpdatePlatformPlaybackState(
      CobaltExtensionMediaSessionPlaybackState state);

  void UpdateMediaSessionState();

  THREAD_CHECKER(thread_checker_);
  const CobaltExtensionMediaSessionApi* extension_;

  const raw_ptr<content::MediaSessionImpl, DanglingUntriaged> media_session_;
  mojo::Receiver<::media_session::mojom::MediaSessionObserver>
      observer_receiver_{this};

  bool is_paused_;
  absl::optional<::media_session::MediaMetadata> metadata_;
  std::vector<::media_session::mojom::MediaSessionAction> actions_;
  std::vector<::media_session::MediaImage> artworks_;
  absl::optional<::media_session::MediaPosition> position_;
};

}  // namespace media_session
}  // namespace cobalt

#endif  // COBALT_BROWSER_MEDIA_SESSION_MEDIA_SESSION_COBALT_H_
