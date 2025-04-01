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

package dev.cobalt.media;

import static dev.cobalt.media.Log.TAG;

import android.app.Activity;
import android.content.Context;
import android.graphics.SurfaceTexture;
import android.util.AttributeSet;
import android.view.Surface;
import android.view.TextureView;
import android.view.ViewGroup.LayoutParams;
import android.widget.FrameLayout;
import dev.cobalt.util.Log;
import dev.cobalt.util.UsedByNative;

/**
 * A Surface view to be used by the video decoder. It informs the Starboard application when the
 * surface is available so that the decoder can get a reference to it.
 */
// public class VideoSurfaceView extends SurfaceView {
//   public VideoSurfaceView(Context context) {
//     super(context);
//     initialize(context);
//   }

//   public VideoSurfaceView(Context context, AttributeSet attrs) {
//     super(context, attrs);
//     initialize(context);
//   }

//   public VideoSurfaceView(Context context, AttributeSet attrs, int defStyleAttr) {
//     super(context, attrs, defStyleAttr);
//     initialize(context);
//   }

//   public VideoSurfaceView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes)
// {
//     super(context, attrs, defStyleAttr, defStyleRes);
//     initialize(context);
//   }

//   private void initialize(Context context) {
//     setBackgroundColor(Color.TRANSPARENT);
//     getHolder().addCallback(new SurfaceHolderCallback(this));
//   }

//   private static native void nativeOnVideoSurfaceCreated(
//       Surface surface, VideoSurfaceView surfaceView);

//   private static native void nativeOnVideoSurfaceCDestroyed(
//       Surface surface, VideoSurfaceView surfaceView);

//   private class SurfaceHolderCallback implements SurfaceHolder.Callback {
//     private VideoSurfaceView surfaceView;

//     public SurfaceHolderCallback(VideoSurfaceView surfaceView) {
//       this.surfaceView = surfaceView;
//     }

//     @Override
//     public void surfaceCreated(SurfaceHolder holder) {
//       nativeOnVideoSurfaceCreated(holder.getSurface(), this.surfaceView);
//       holder.setFormat(PixelFormat.TRANSPARENT);
//     }

//     @Override
//     public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}

//     @Override
//     public void surfaceDestroyed(SurfaceHolder holder) {
//       nativeOnVideoSurfaceCDestroyed(holder.getSurface(), this.surfaceView);
//     }
//   }

//   @UsedByNative
//   public void setVideoSurfaceBounds(final int x, final int y, final int width, final int height)
// {
//     if (width == 0 || height == 0) {
//       // The SurfaceView should be covered by our UI layer in this case.
//       return;
//     }
//     VideoSurfaceView surfaceView = this;
//     getActivity()
//         .runOnUiThread(
//             new Runnable() {
//               @Override
//               public void run() {
//                 LayoutParams layoutParams = surfaceView.getLayoutParams();
//                 // Since videoSurfaceView is added directly to the Activity's content view, which
// is
//                 // a
//                 // FrameLayout, we expect its layout params to become FrameLayout.LayoutParams.
//                 if (layoutParams instanceof FrameLayout.LayoutParams) {
//                   ((FrameLayout.LayoutParams) layoutParams).setMargins(x, y, x + width, y +
// height);
//                 } else {
//                   Log.w(
//                       TAG,
//                       "Unexpected video surface layout params class "
//                           + layoutParams.getClass().getName());
//                 }
//                 layoutParams.width = width;
//                 layoutParams.height = height;
//                 // Even though as a NativeActivity we're not using the Android UI framework, by
//                 // setting
//                 // the  layout params it will force a layout to be requested. That will cause the
//                 // SurfaceView to position its underlying Surface to match the screen coordinates
// of
//                 // where the view would be in a UI layout and to set the surface transform matrix
// to
//                 // match the view's size.
//                 surfaceView.setLayoutParams(layoutParams);
//               }
//             });
//   }

//   private Activity getActivity() {
//     Context context = getContext();
//     return (Activity) context;
//   }
// }

public class VideoSurfaceView extends TextureView {
  public Surface surface = null;

  public VideoSurfaceView(Context context) {
    super(context);
    initialize(context);
  }

  public VideoSurfaceView(Context context, AttributeSet attrs) {
    super(context, attrs);
    initialize(context);
  }

  public VideoSurfaceView(Context context, AttributeSet attrs, int defStyleAttr) {
    super(context, attrs, defStyleAttr);
    initialize(context);
  }

  public VideoSurfaceView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
    super(context, attrs, defStyleAttr, defStyleRes);
    initialize(context);
  }

  private void initialize(Context context) {
    setSurfaceTextureListener(new SurfaceTextureListener(this));
  }

  private static native void nativeOnVideoSurfaceCreated(
      Surface surface, VideoSurfaceView surfaceView);

  private static native void nativeOnVideoSurfaceCDestroyed(
      Surface surface, VideoSurfaceView surfaceView);

  private class SurfaceTextureListener implements TextureView.SurfaceTextureListener {
    private VideoSurfaceView surfaceView;

    public SurfaceTextureListener(VideoSurfaceView surfaceView) {
      this.surfaceView = surfaceView;
    }

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture surfaceTexture, int width, int height) {
      this.surfaceView.surface = new Surface(surfaceTexture);
      nativeOnVideoSurfaceCreated(this.surfaceView.surface, this.surfaceView);
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture, int width, int height) {}

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture surfaceTexture) {
      nativeOnVideoSurfaceCDestroyed(this.surfaceView.surface, this.surfaceView);
      return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture surfaceTexture) {}
  }

  @UsedByNative
  public void setVideoSurfaceBounds(final int x, final int y, final int width, final int height) {
    if (width == 0 || height == 0) {
      // The SurfaceView should be covered by our UI layer in this case.
      return;
    }
    VideoSurfaceView surfaceView = this;
    getActivity()
        .runOnUiThread(
            new Runnable() {
              @Override
              public void run() {
                LayoutParams layoutParams = surfaceView.getLayoutParams();
                // Since videoSurfaceView is added directly to the Activity's content view, which is
                // a
                // FrameLayout, we expect its layout params to become FrameLayout.LayoutParams.
                if (layoutParams instanceof FrameLayout.LayoutParams) {
                  ((FrameLayout.LayoutParams) layoutParams).setMargins(x, y, x + width, y + height);
                } else {
                  Log.w(
                      TAG,
                      "Unexpected video surface layout params class "
                          + layoutParams.getClass().getName());
                }
                layoutParams.width = width;
                layoutParams.height = height;
                // Even though as a NativeActivity we're not using the Android UI framework, by
                // setting
                // the  layout params it will force a layout to be requested. That will cause the
                // SurfaceView to position its underlying Surface to match the screen coordinates of
                // where the view would be in a UI layout and to set the surface transform matrix to
                // match the view's size.
                surfaceView.setLayoutParams(layoutParams);
              }
            });
  }

  private Activity getActivity() {
    Context context = getContext();
    return (Activity) context;
  }
}
