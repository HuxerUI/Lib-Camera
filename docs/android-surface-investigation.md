# Android Surface Preview Investigation

Status: black-preview fix verified on TAS_AN00. With the HuxerUI Android texture-layer first-draw fix, the user confirmed that both shared CameraPreview instances display camera frames, including after stop/restart and front/back switching. The user subsequently updated the installed SDK. The example was rebuilt against it, its packaged native library was verified to match, and the user confirmed normal operation.

## Environment

- Device: Huawei TAS_AN00, serial GCL0220515007739.
- Installed HuxerUI SDK: `/Users/harvenguo/Library/Developer/HuxerUI`.
- CameraX: 1.5.3; arm64-v8a Debug application.
- Camera permission: granted.
- Activity decor view `isHardwareAccelerated()`: true.
- Before the fix, the SDK and APK contained byte-identical arm64-v8a `libhuxerui.so` files, SHA-256 `371ea10ddd3febde8d5efe44849a0c41a2b3238bd8d2140c29f8001634149b12`.

## Observations Before the Fix

| Input | CameraPreview / Canvas | Direct SDK Image |
| --- | --- | --- |
| CameraX Surface, 1440×1080 | Black | Black |
| Synthetic green Surface, 64×64 | Black | Black |
| Synthetic green BitmapTexture, 64×64 | Green | Green |

The user confirmed each visual comparison on the device. The synthetic Surface test does not open a camera and uses no rotation, mirroring, or crop. Both synthetic textures cross the same PlatformPayload bridge and use the same component bounds and background modifiers.

CameraPreview received a non-null texture and a 320×240 paint area. The synthetic Surface reached `Revision() == 1` and `IsActive() == true`. Three subsequent application state updates caused fresh compositions and paint recordings with that same published texture; the image remained black. The equivalent BitmapTexture also reported revision 1 and active visibility, and displayed green.

An earlier, separate failure on restart was caused by registering the same PlatformChannel event in every Start call. That is fixed by subscribing once at backend creation. A four-run device sequence passed stop/restart, live facing changes, and advancing camera texture revisions. Those results establish lifecycle and publication behavior, not visible GPU output.

## Synthetic Surface Reproduction

Create the producer through the installed public SDK on the module's main-thread JNI callback, retaining the returned texture capability:

```cpp
auto texture = huxerui::android::SurfaceStreamTexture::Create(env, {64, 64}, 64, 64);
auto surface = texture->Surface(env);
```

On the Java side, draw and post a known opaque frame to that Surface:

```java
Canvas canvas = surface.lockCanvas(null);
canvas.drawColor(Color.GREEN);
surface.unlockCanvasAndPost(canvas);
```

Publish the retained texture via the module's existing snapshot event, with rotation 0 and mirroring false. Display it both through CameraPreview and directly through `Image(texture)`. Keep the producer alive while inspecting the result. Call Finish only after the producer has stopped using the Surface.

For the control test, create an immutable ARGB_8888 Bitmap, fill it with `Color.GREEN`, and publish it through `android::BitmapTexture::Publish(env, bitmap)`. Preserve the same event transport and display paths. Both controls should display green; on this device only BitmapTexture does.

All temporary synthetic producers, diagnostic JNI methods, automatic starts, repaint probes, and Image substitutions were removed after the checks. The normal CameraX implementation and manual Start example are restored. No Bitmap fallback was added.

## Root Cause and Fix

The black-preview TextureViews were attached, visible, and measured, but `isAvailable()` remained false. In `platform/android/huxerui/src/main/cpp/android_gpu_texture.cpp`, `AndroidTextureLayers::Draw` returned whenever `has_presentation` was false. A newly mounted TextureView requires its first `drawChild` pass to create its display Surface; without that Surface, the platform layer cannot present a frame. The guard therefore prevented initialization from progressing.

The guard now returns only when `!layer.has_presentation && layer.output`. A new child with no output Surface can perform its first draw, while an existing output still remains hidden until its current frame has been presented. No public API or camera transport changes were needed.

The user reported that the framework's SurfaceStreamTexture example worked before this fix. The camera case adds its texture layers after asynchronous output publication; a difference in mounting timing is a plausible explanation, but has not been independently verified as the reason the example escaped the issue.

## Verification

- Framework Android Debug native builds passed for arm64-v8a and x86_64.
- The camera example built against a separate SDK directory containing matching source headers, Java AAR, and the patched arm64-v8a native library.
- The APK's native library matches that test SDK, SHA-256 `f37dda0a9462dcd50cf88499b58f197fe93826dd3bba6db8bf0cac38c4cd28aa`.
- On TAS_AN00, the user confirmed visible output in both shared previews, successful stop/start, and successful front/back switching.
- The framework diff is confined to the first-draw condition and its explanation; `git diff --check` passed.

Replacing only the native library was insufficient for this checkout: it includes newer Java bridge types than the installed SDK. That mismatched test package failed during bridge initialization. Rebuilding with matching SDK components resolved the packaging mismatch. The initial verification did not overwrite the installed SDK. The user later updated it, and a fresh build against that installation also passed interactive preview checks.

## Remaining Work

The user subsequently confirmed all four display orientations, front-camera mirroring, Cover/Contain, and camera background recovery on TAS_AN00. The patched framework external_texture example also passed user checks for BitmapTexture, GlTexture, and SurfaceStreamTexture animation, pause/resume, overlays, rounded clipping, and background return. A separate Home/return check retained the same process ID and recorded no publication or GPU presentation errors. The synthetic green Surface comparisons above record the original failure. These results do not establish acceptance on other Android devices.

Review also found oversized internal texture-layer targets: the 320×240 and 160×90 previews each produced a 3240×4320 TextureView in the diagnostic run. CameraPreview records an intrinsic-size draw destination and then scales the Canvas, while the Android host sizes its texture layer from the untransformed destination. This may increase GPU memory and rendering cost. It is separate from the first-draw fix and has not been changed pending discussion.
