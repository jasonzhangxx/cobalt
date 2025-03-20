// Copyright 2017 The Cobalt Authors. All Rights Reserved.
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

#include "starboard/android/shared/video_window.h"

#include <jni.h>
#include <vector>

#include "starboard/android/shared/jni_env_ext.h"
#include "starboard/common/log.h"
#include "starboard/common/mutex.h"
#include "starboard/common/once.h"
#include "starboard/configuration.h"
#include "starboard/shared/gles/gl_call.h"

namespace starboard {
namespace android {
namespace shared {

namespace {

struct VideoSurfaceEntry {
  VideoSurfaceEntry(jobject surface_view, jobject surface) {
    j_surface_view = surface_view;
    j_surface = surface;
  }

  jobject j_surface_view;
  jobject j_surface;
  VideoSurfaceHolder* surface_holder = nullptr;
};

// Global video surface pointer mutex.
SB_ONCE_INITIALIZE_FUNCTION(Mutex, GetViewSurfaceMutex);
// Global pointer to video surfaces and video surface pointer holders.
std::vector<VideoSurfaceEntry> g_video_surfaces;

}  // namespace

extern "C" SB_EXPORT_PLATFORM void
Java_dev_cobalt_media_VideoSurfaceView_nativeOnVideoSurfaceCreated(
    JNIEnv* env,
    jobject unused_this,
    jobject surface,
    jobject surface_view) {
  SB_DCHECK(surface);

  ScopedLock lock(*GetViewSurfaceMutex());
  g_video_surfaces.emplace_back(env->NewGlobalRef(surface_view),
                                env->NewGlobalRef(surface));

  SB_LOG(ERROR) << "Received a surface, currently we have "
                << g_video_surfaces.size() << " surfaces.";
}

extern "C" SB_EXPORT_PLATFORM void
Java_dev_cobalt_media_VideoSurfaceView_nativeOnVideoSurfaceCDestroyed(
    JNIEnv* env,
    jobject unused_this,
    jobject surface,
    jobject surface_view) {
  for (auto it = g_video_surfaces.begin(); it != g_video_surfaces.end(); ++it) {
    if (env->IsSameObject(surface, it->j_surface)) {
      if (it->surface_holder) {
        it->surface_holder->OnSurfaceDestroyed();
      }
      env->DeleteGlobalRef(it->j_surface_view);
      env->DeleteGlobalRef(it->j_surface);
      g_video_surfaces.erase(it);
      SB_LOG(ERROR) << "Destroyed a surface, currently we have "
                    << g_video_surfaces.size() << " surfaces.";
      return;
    }
  }
  SB_NOTREACHED();
}

jobject VideoSurfaceHolder::AcquireVideoSurface() {
  ScopedLock lock(*GetViewSurfaceMutex());
  for (auto it = g_video_surfaces.begin(); it != g_video_surfaces.end(); ++it) {
    if (!it->surface_holder) {
      it->surface_holder = this;
      return it->j_surface;
    }
  }
  SB_LOG(ERROR) << "No available video surface right now, currently we have "
                << g_video_surfaces.size() << " surfaces.";
  return NULL;
}

void VideoSurfaceHolder::ReleaseVideoSurface() {
  ScopedLock lock(*GetViewSurfaceMutex());
  for (auto it = g_video_surfaces.begin(); it != g_video_surfaces.end(); ++it) {
    if (it->surface_holder == this) {
      it->surface_holder = nullptr;
      return;
    }
  }
}

void VideoSurfaceHolder::SetBounds(int z_index,
                                   int x,
                                   int y,
                                   int width,
                                   int height) {
  ScopedLock lock(*GetViewSurfaceMutex());
  for (auto it = g_video_surfaces.begin(); it != g_video_surfaces.end(); ++it) {
    if (it->surface_holder == this) {
      JniEnvExt* env = JniEnvExt::Get();
      env->CallVoidMethodOrAbort(it->j_surface_view, "setVideoSurfaceBounds",
                                 "(IIII)V", x, y, width, height);
      return;
    }
  }
}

}  // namespace shared
}  // namespace android
}  // namespace starboard
