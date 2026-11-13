package org.huxerui.lib.camera;

import android.content.Context;
import android.graphics.PointF;
import android.graphics.Rect;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.Range;
import android.util.Size;
import android.view.Choreographer;
import android.view.Surface;
import android.view.WindowManager;

import androidx.annotation.NonNull;
import androidx.camera.camera2.interop.Camera2CameraInfo;
import androidx.camera.core.Camera;
import androidx.camera.core.CameraControl;
import androidx.camera.core.CameraInfo;
import androidx.camera.core.CameraSelector;
import androidx.camera.core.CameraState;
import androidx.camera.core.ExposureState;
import androidx.camera.core.ExperimentalSessionConfig;
import androidx.camera.core.FocusMeteringAction;
import androidx.camera.core.FocusMeteringResult;
import androidx.camera.core.Preview;
import androidx.camera.core.ResolutionInfo;
import androidx.camera.core.SessionConfig;
import androidx.camera.core.ImageCapture;
import androidx.camera.core.ImageCaptureException;
import androidx.camera.core.ImageProxy;
import androidx.camera.core.SurfaceRequest;
import androidx.camera.core.SurfaceOrientedMeteringPointFactory;
import androidx.camera.core.TorchState;
import androidx.camera.core.UseCase;
import androidx.camera.core.ZoomState;
import androidx.camera.core.resolutionselector.ResolutionSelector;
import androidx.camera.core.resolutionselector.ResolutionStrategy;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.lifecycle.Lifecycle;
import androidx.lifecycle.LifecycleOwner;
import androidx.lifecycle.LifecycleRegistry;
import androidx.lifecycle.Observer;

import com.google.common.util.concurrent.ListenableFuture;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class CameraModule implements HuxerUIPlatformModule.Factory {
    @androidx.camera.camera2.interop.ExperimentalCamera2Interop
    private static boolean usesRealtimeTimestamps(CameraInfo info) {
        try {
            Integer source = Camera2CameraInfo.from(info)
                    .getCameraCharacteristic(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE);
            return source != null && source == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
        } catch (IllegalArgumentException unavailable) {
            return false;
        }
    }

    @Override
    public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                                       HuxerUIPlatformChannel.Events events) {
        return new Session(context, events);
    }

    private static final class Session implements HuxerUIPlatformModule, LifecycleOwner,
            DisplayManager.DisplayListener {
        private final Context context;
        private final HuxerUIPlatformChannel.Events events;
        private final Handler main = new Handler(Looper.getMainLooper());
        private final Executor executor = command -> main.post(command);
        private final LifecycleRegistry lifecycle = new LifecycleRegistry(this);
        private final DisplayManager displays;
        private final Set<Output> outputs = new HashSet<>();
        private final ArrayList<HuxerUIPlatformChannel.Result> stops = new ArrayList<>();
        private ProcessCameraProvider provider;
        private Preview preview;
        private ImageCapture photoOutput;
        private SessionConfig rateSession;
        private final ExecutorService photoExecutor = Executors.newSingleThreadExecutor();
        private boolean photoPending;
        private Camera camera;
        private Observer<CameraState> cameraObserver;
        private Observer<Integer> torchObserver;
        private HuxerUIPlatformChannel.Result pendingZoom;
        private float requestedZoomRatio;
        private final Choreographer.FrameCallback zoomFrame = time -> applyZoomRatio();
        private Output current;
        private CameraSurface.FrameThread frameThread;
        private boolean frameThreadStopping;
        private long generation;
        private long run;
        private long frames;
        private boolean active;
        private boolean opened;
        private boolean supportsPointControl;
        private boolean disposed;
        private String facing;

        Session(Context context, HuxerUIPlatformChannel.Events events) {
            this.context = context;
            this.events = events;
            displays = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_CREATE);
        }

        @NonNull
        @Override
        public Lifecycle getLifecycle() { return lifecycle; }

        @Override
        public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload arguments,
                                                          HuxerUIPlatformChannel.Result result) {
            if (disposed) {
                result.fail("camera/closed", "The camera session is closed.", PlatformPayload.nullValue());
            } else if (method.equals("start")) {
                if (active || photoPending || !outputs.isEmpty() || frameThreadStopping) {
                    result.fail("camera/busy", "The previous camera output is still stopping.", PlatformPayload.nullValue());
                    return null;
                }
                run = arguments.requireField("run").requireInt64();
                frames = arguments.requireField("frames").requireInt64();
                PlatformPayload requested = arguments.requireField("facing");
                long width = arguments.requireField("captureWidth").requireInt64();
                long height = arguments.requireField("captureHeight").requireInt64();
                PlatformPayload rate = arguments.requireField("captureFps");
                start(requested.isNull() ? null : requested.requireString(), width, height,
                        rate.isNull() ? null : rate.requireDouble());
                result.complete(PlatformPayload.nullValue());
            } else if (method.equals("capturePhoto")) {
                capturePhoto(arguments, result);
            } else if (method.equals("readZoom")) {
                ZoomState zoom = isCurrentCamera(arguments) ? zoomState() : null;
                result.complete(zoom == null ? PlatformPayload.nullValue() : zoomPayload(zoom));
            } else if (method.equals("setZoomRatio")) {
                setZoomRatio(arguments, result);
            } else if (method.equals("readExposureCompensation")) {
                ExposureState exposure = isCurrentCamera(arguments) ? exposureState() : null;
                result.complete(exposure == null ? PlatformPayload.nullValue()
                        : exposurePayload(exposure, exposure.getExposureCompensationIndex()));
            } else if (method.equals("setExposureCompensation")) {
                setExposureCompensation(arguments, result);
            } else if (method.equals("observeLighting")) {
                if (isCurrentCamera(arguments)) observeLighting();
                result.complete(PlatformPayload.nullValue());
            } else if (method.equals("setTorchEnabled")) {
                setTorchEnabled(arguments, result);
            } else if (method.equals("focusAndMeter")) {
                focusAndMeter(arguments, result);
            } else if (method.equals("stop")) {
                stops.add(result);
                stop();
            } else {
                result.fail("camera/unknown-method", "Unknown camera method.", PlatformPayload.nullValue());
            }
            return null;
        }

        private void start(String requested, long width, long height, Double fps) {
            active = true;
            opened = false;
            facing = null;
            long token = ++generation;
            try {
                ListenableFuture<ProcessCameraProvider> future = ProcessCameraProvider.getInstance(context);
                future.addListener(() -> {
                    if (!active || token != generation) return;
                    try {
                        provider = future.get();
                        bind(requested, width, height, fps, token);
                    } catch (SecurityException error) {
                        fail("permission-denied", error.toString());
                    } catch (Exception error) {
                        fail("configuration-failed", error.toString());
                    }
                }, executor);
            } catch (Exception error) {
                fail("unavailable", error.toString());
            }
        }

        private void bind(String requested, long width, long height, Double fps, long token) {
            CameraInfo selected = null;
            for (CameraInfo candidate : provider.getAvailableCameraInfos()) {
                int lens = candidate.getLensFacing();
                if (requested == null) {
                    if (selected == null || lens == CameraSelector.LENS_FACING_BACK) selected = candidate;
                    if (lens == CameraSelector.LENS_FACING_BACK) break;
                } else if ((requested.equals("front") && lens == CameraSelector.LENS_FACING_FRONT)
                        || (requested.equals("back") && lens == CameraSelector.LENS_FACING_BACK)) {
                    selected = candidate;
                    break;
                }
            }
            if (selected == null) {
                fail("device-not-found", "No camera matches the requested facing.");
                return;
            }
            int lens = selected.getLensFacing();
            facing = lens == CameraSelector.LENS_FACING_FRONT ? "front"
                    : lens == CameraSelector.LENS_FACING_BACK ? "back" : null;
            photoOutput = new ImageCapture.Builder().setJpegQuality(100)
                    .setTargetRotation(displayRotation()).build();
            bindOutputs(selected, width, height);
            if (fps != null) applyFrameRate(selected, fps, width != 0);
            preview.setSurfaceProvider(executor, request -> {
                if (!active || token != generation) {
                    request.willNotProvideSurface();
                    return;
                }
                Output output = new Output(request, token);
                outputs.add(output);
                request.addRequestCancellationListener(executor, () -> {
                    if (!output.provided) output.release();
                });
                request.setTransformationInfoListener(executor, output::configure);
            });
            cameraObserver = state -> {
                if (!active || token != generation) return;
                CameraState.StateError error = state.getError();
                if (error != null) {
                    String code;
                    switch (error.getCode()) {
                        case CameraState.ERROR_CAMERA_IN_USE:
                        case CameraState.ERROR_MAX_CAMERAS_IN_USE: code = "device-busy"; break;
                        case CameraState.ERROR_STREAM_CONFIG: code = "configuration-failed"; break;
                        case CameraState.ERROR_CAMERA_DISABLED: code = "permission-denied"; break;
                        default: code = "capture-failed"; break;
                    }
                    fail(code, "CameraX error " + error.getCode()
                            + (error.getCause() == null ? "" : ": " + error.getCause()));
                } else if (state.getType() == CameraState.Type.OPEN && !opened) {
                    supportsPointControl = camera.getCameraInfo().isFocusMeteringSupported(meteringAction(0.5f, 0.5f));
                    opened = true;
                    emit(null);
                }
            };
            camera.getCameraInfo().getCameraState().observeForever(cameraObserver);
            displays.registerDisplayListener(this, main);
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_RESUME);
        }

        private Preview makePreview(long width, long height, Size exactSize) {
            Preview.Builder builder = new Preview.Builder().setTargetRotation(displayRotation());
            if (exactSize != null || width != 0) {
                builder.setResolutionSelector(new ResolutionSelector.Builder()
                        .setResolutionStrategy(ResolutionStrategy.HIGHEST_AVAILABLE_STRATEGY)
                        .setResolutionFilter((sizes, rotation) -> {
                            if (exactSize != null) {
                                return sizes.contains(exactSize) ? Collections.singletonList(exactSize) : Collections.emptyList();
                            }
                            ArrayList<Size> ordered = new ArrayList<>(sizes);
                            Collections.sort(ordered, (left, right) -> Double.compare(
                                    sizeDistance(left, width, height), sizeDistance(right, width, height)));
                            return ordered;
                        }).build());
            }
            return builder.build();
        }

        private static double sizeDistance(Size size, long width, long height) {
            int shorter = Math.min(size.getWidth(), size.getHeight());
            int longer = Math.max(size.getWidth(), size.getHeight());
            return Math.abs(Math.log((double) shorter / Math.min(width, height)))
                    + Math.abs(Math.log((double) longer / Math.max(width, height)));
        }

        private void bindOutputs(CameraInfo selected, long width, long height) {
            preview = makePreview(width, height, null);
            CameraSelector selector = selected.getCameraSelector();
            try {
                camera = provider.bindToLifecycle(this, selector, preview, photoOutput);
                return;
            } catch (IllegalArgumentException unsupported) {
                provider.unbind(preview, photoOutput);
            }
            if (width != 0) {
                Preview defaults = makePreview(0, 0, null);
                try {
                    camera = provider.bindToLifecycle(this, selector, defaults, photoOutput);
                    preview = defaults;
                    return;
                } catch (IllegalArgumentException unsupported) {
                    provider.unbind(defaults, photoOutput);
                }
            }
            photoOutput = null;
            try {
                camera = provider.bindToLifecycle(this, selector, preview);
            } catch (IllegalArgumentException unsupported) {
                if (width == 0) throw unsupported;
                provider.unbind(preview);
                preview = makePreview(0, 0, null);
                camera = provider.bindToLifecycle(this, selector, preview);
            }
        }

        @ExperimentalSessionConfig
        private void applyFrameRate(CameraInfo selected, double fps, boolean keepSize) {
            // Resolve size with the same photo use case before applying any frame rate constraint.
            ResolutionInfo resolution = preview.getResolutionInfo();
            if (keepSize && resolution == null) return;
            Preview candidate = makePreview(0, 0, keepSize ? resolution.getResolution() : null);
            UseCase[] useCases = photoOutput == null ? new UseCase[]{candidate} : new UseCase[]{candidate, photoOutput};
            Range<Integer> range = null;
            try {
                double bestDistance = Double.POSITIVE_INFINITY;
                int bestSpan = Integer.MAX_VALUE;
                for (Range<Integer> supported : selected.getSupportedFrameRateRanges(new SessionConfig(useCases))) {
                    int lower = supported.getLower();
                    int upper = supported.getUpper();
                    if (lower <= 0) continue;
                    double distance = Math.abs(fps - Math.max(lower, Math.min(fps, upper)));
                    int span = upper - lower;
                    if (range == null || distance < bestDistance || (distance == bestDistance
                            && (span < bestSpan || (span == bestSpan && lower < range.getLower())))) {
                        range = supported;
                        bestDistance = distance;
                        bestSpan = span;
                    }
                }
            } catch (IllegalArgumentException unsupported) {
                return;
            }
            if (range == null) return;
            SessionConfig config = new SessionConfig.Builder(useCases).setFrameRateRange(range).build();
            CameraSelector selector = selected.getCameraSelector();
            if (photoOutput != null) provider.unbind(preview, photoOutput);
            else provider.unbind(preview);
            try {
                camera = provider.bindToLifecycle(this, selector, config);
                preview = candidate;
                rateSession = config;
            } catch (IllegalArgumentException unsupported) {
                provider.unbind(config);
                camera = photoOutput == null ? provider.bindToLifecycle(this, selector, preview)
                        : provider.bindToLifecycle(this, selector, preview, photoOutput);
            }
        }

        private boolean isCurrentCamera(PlatformPayload arguments) {
            return active && opened && camera != null && arguments.requireField("run").requireInt64() == run;
        }

        private ZoomState zoomState() {
            ZoomState zoom = camera.getCameraInfo().getZoomState().getValue();
            return zoom != null && zoom.getMaxZoomRatio() > zoom.getMinZoomRatio() ? zoom : null;
        }

        private static PlatformPayload zoomPayload(ZoomState zoom) {
            Map<String, PlatformPayload> values = new HashMap<>();
            values.put("ratio", PlatformPayload.doubleValue(zoom.getZoomRatio()));
            values.put("min", PlatformPayload.doubleValue(zoom.getMinZoomRatio()));
            values.put("max", PlatformPayload.doubleValue(zoom.getMaxZoomRatio()));
            return PlatformPayload.object(values);
        }

        private void setZoomRatio(PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            if (!isCurrentCamera(arguments)) {
                result.fail("not-ready", "Start the camera before adjusting zoom.", PlatformPayload.nullValue());
                return;
            }
            requestedZoomRatio = (float) arguments.requireField("ratio").requireDouble();
            HuxerUIPlatformChannel.Result previous = pendingZoom;
            pendingZoom = result;
            if (previous == null) {
                Choreographer.getInstance().postFrameCallback(zoomFrame);
            } else {
                previous.fail("interrupted", "A newer zoom request superseded this adjustment.", PlatformPayload.nullValue());
            }
        }

        private void applyZoomRatio() {
            HuxerUIPlatformChannel.Result result = pendingZoom;
            pendingZoom = null;
            if (result == null) return;
            ZoomState zoom = zoomState();
            if (zoom == null) {
                result.fail("unavailable", "Camera zoom is unavailable.", PlatformPayload.nullValue());
                return;
            }
            float target = Math.max(zoom.getMinZoomRatio(), Math.min(requestedZoomRatio, zoom.getMaxZoomRatio()));
            long token = generation;
            try {
                ListenableFuture<Void> applied = camera.getCameraControl().setZoomRatio(target);
                applied.addListener(() -> {
                    if (!active || token != generation) {
                        result.fail("interrupted", "Camera zoom was interrupted.", PlatformPayload.nullValue());
                        return;
                    }
                    try {
                        applied.get();
                        ZoomState current = zoomState();
                        if (current == null) {
                            result.fail("unavailable", "Camera zoom is unavailable.", PlatformPayload.nullValue());
                        } else {
                            result.complete(zoomPayload(current));
                        }
                    } catch (Exception error) {
                        String code = error.getCause() instanceof CameraControl.OperationCanceledException
                                ? "interrupted" : "capture-failed";
                        result.fail(code, error.toString(), PlatformPayload.nullValue());
                    }
                }, executor);
            } catch (RuntimeException error) {
                result.fail("capture-failed", error.toString(), PlatformPayload.nullValue());
            }
        }

        private ExposureState exposureState() {
            ExposureState exposure = camera.getCameraInfo().getExposureState();
            return exposure.isExposureCompensationSupported()
                    && exposure.getExposureCompensationStep().doubleValue() > 0.0 ? exposure : null;
        }

        private static PlatformPayload exposurePayload(ExposureState exposure, int index) {
            double step = exposure.getExposureCompensationStep().doubleValue();
            Range<Integer> range = exposure.getExposureCompensationRange();
            Map<String, PlatformPayload> values = new HashMap<>();
            values.put("ev", PlatformPayload.doubleValue(index * step));
            values.put("min", PlatformPayload.doubleValue(range.getLower() * step));
            values.put("max", PlatformPayload.doubleValue(range.getUpper() * step));
            values.put("step", PlatformPayload.doubleValue(step));
            return PlatformPayload.object(values);
        }

        private void setExposureCompensation(PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            if (!isCurrentCamera(arguments)) {
                result.fail("not-ready", "Start the camera before adjusting exposure compensation.",
                        PlatformPayload.nullValue());
                return;
            }
            ExposureState exposure = exposureState();
            if (exposure == null) {
                result.fail("unavailable", "Camera exposure compensation is unavailable.", PlatformPayload.nullValue());
                return;
            }
            Range<Integer> range = exposure.getExposureCompensationRange();
            double ev = arguments.requireField("ev").requireDouble();
            long index = Math.round(ev / exposure.getExposureCompensationStep().doubleValue());
            int target = (int) Math.max(range.getLower(), Math.min(index, range.getUpper()));
            long token = generation;
            try {
                ListenableFuture<Integer> applied = camera.getCameraControl().setExposureCompensationIndex(target);
                applied.addListener(() -> {
                    if (!active || token != generation) {
                        result.fail("interrupted", "Camera exposure compensation was interrupted.",
                                PlatformPayload.nullValue());
                        return;
                    }
                    try {
                        result.complete(exposurePayload(exposure, applied.get()));
                    } catch (Exception error) {
                        String code = error.getCause() instanceof CameraControl.OperationCanceledException
                                ? "interrupted" : "capture-failed";
                        result.fail(code, error.toString(), PlatformPayload.nullValue());
                    }
                }, executor);
            } catch (RuntimeException error) {
                result.fail("capture-failed", error.toString(), PlatformPayload.nullValue());
            }
        }

        private FocusMeteringAction meteringAction(float x, float y) {
            return new FocusMeteringAction.Builder(new SurfaceOrientedMeteringPointFactory(1, 1)
                    .createPoint(x, y), FocusMeteringAction.FLAG_AF | FocusMeteringAction.FLAG_AE).build();
        }

        private void focusAndMeter(PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            if (!isCurrentCamera(arguments) || current == null || current.invalidated || current.output == null) {
                result.fail("not-ready", "The camera preview is not ready.", PlatformPayload.nullValue());
                return;
            }
            float x = (float) arguments.requireField("x").requireDouble();
            float y = (float) arguments.requireField("y").requireDouble();
            PointF point = current.gpu.bufferPoint(x, y);
            if (point == null) {
                result.fail("not-ready", "The camera preview is not ready.", PlatformPayload.nullValue());
                return;
            }
            FocusMeteringAction action = meteringAction(point.x, point.y);
            CameraControl control = camera.getCameraControl();
            if (!supportsPointControl || !camera.getCameraInfo().isFocusMeteringSupported(action)) {
                result.fail("unavailable", "Point focus and metering are unavailable.", PlatformPayload.nullValue());
                return;
            }
            long token = generation;
            try {
                ListenableFuture<FocusMeteringResult> applied = control.startFocusAndMetering(action);
                applied.addListener(() -> {
                    if (!active || token != generation) {
                        result.fail("interrupted", "Camera focus was interrupted.", PlatformPayload.nullValue());
                        return;
                    }
                    try {
                        applied.get();
                        result.complete(PlatformPayload.nullValue());
                    } catch (Exception error) {
                        String code = error.getCause() instanceof CameraControl.OperationCanceledException
                                ? "interrupted" : "capture-failed";
                        result.fail(code, error.toString(), PlatformPayload.nullValue());
                    }
                }, executor);
            } catch (RuntimeException error) {
                result.fail("capture-failed", error.toString(), PlatformPayload.nullValue());
            }
        }

        private void observeLighting() {
            CameraInfo info = camera.getCameraInfo();
            if (info.hasFlashUnit() && torchObserver == null) {
                torchObserver = state -> emitLighting();
                info.getTorchState().observeForever(torchObserver);
            }
            emitLighting();
        }

        private boolean torchEnabled() {
            return Integer.valueOf(TorchState.ON).equals(camera.getCameraInfo().getTorchState().getValue());
        }

        private void emitLighting() {
            if (!active || !opened || camera == null) return;
            boolean hasTorch = camera.getCameraInfo().hasFlashUnit();
            Map<String, PlatformPayload> lighting = new HashMap<>();
            lighting.put("run", PlatformPayload.int64(run));
            lighting.put("hasFlash", PlatformPayload.booleanValue(hasTorch && photoOutput != null));
            lighting.put("hasTorch", PlatformPayload.booleanValue(hasTorch));
            lighting.put("torchEnabled", PlatformPayload.booleanValue(hasTorch && torchEnabled()));
            events.emit("lighting", PlatformPayload.object(lighting));
        }

        private void setTorchEnabled(PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            if (!isCurrentCamera(arguments)) {
                result.fail("not-ready", "The camera is not running.", PlatformPayload.nullValue());
                return;
            }
            if (!camera.getCameraInfo().hasFlashUnit()) {
                result.fail("unavailable", "The camera torch is unavailable.", PlatformPayload.nullValue());
                return;
            }
            boolean enabled = arguments.requireField("enabled").requireBoolean();
            long token = generation;
            try {
                ListenableFuture<Void> applied = camera.getCameraControl().enableTorch(enabled);
                applied.addListener(() -> {
                    if (!active || token != generation) {
                        result.fail("interrupted", "The camera torch adjustment was interrupted.", PlatformPayload.nullValue());
                        return;
                    }
                    try {
                        applied.get();
                        emitLighting();
                        result.complete(PlatformPayload.nullValue());
                    } catch (Exception error) {
                        String code = error.getCause() instanceof CameraControl.OperationCanceledException
                                ? "interrupted" : "capture-failed";
                        result.fail(code, error.toString(), PlatformPayload.nullValue());
                    }
                }, executor);
            } catch (RuntimeException error) {
                result.fail("capture-failed", error.toString(), PlatformPayload.nullValue());
            }
        }

        private void capturePhoto(PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            if (!active || !opened || arguments.requireField("run").requireInt64() != run) {
                result.fail("not-ready", "Start the camera before taking a photo.", PlatformPayload.nullValue());
                return;
            }
            if (photoPending) {
                result.fail("operation-in-progress", "A photo is still being processed.", PlatformPayload.nullValue());
                return;
            }
            if (photoOutput == null) {
                result.fail("unavailable", "This camera cannot combine preview and still photos.", PlatformPayload.nullValue());
                return;
            }
            int quality = (int) arguments.requireField("quality").requireInt64();
            boolean mirror = arguments.requireField("mirror").requireBoolean();
            String flash = arguments.requireField("flash").requireString();
            int flashMode = flash.equals("on") ? ImageCapture.FLASH_MODE_ON
                    : flash.equals("auto") ? ImageCapture.FLASH_MODE_AUTO : ImageCapture.FLASH_MODE_OFF;
            if (flashMode != ImageCapture.FLASH_MODE_OFF && (!camera.getCameraInfo().hasFlashUnit() || torchEnabled())) {
                result.fail("unavailable", "The requested flash mode is unavailable.", PlatformPayload.nullValue());
                return;
            }
            long token = generation;
            photoPending = true;
            try {
                photoOutput.setTargetRotation(displayRotation());
                photoOutput.setFlashMode(flashMode);
                photoOutput.takePicture(photoExecutor, new ImageCapture.OnImageCapturedCallback() {
                    @Override public void onCaptureSuccess(@NonNull ImageProxy image) {
                        byte[] encoded = null;
                        String failure = null;
                        Bitmap source = null;
                        Bitmap upright = null;
                        try {
                            ByteBuffer buffer = image.getPlanes()[0].getBuffer();
                            byte[] jpeg = new byte[buffer.remaining()];
                            buffer.get(jpeg);
                            source = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length);
                            if (source == null) throw new IllegalStateException("Cannot decode the captured JPEG.");
                            Matrix transform = new Matrix();
                            transform.postRotate(image.getImageInfo().getRotationDegrees());
                            if (mirror) transform.postScale(-1, 1);
                            upright = Bitmap.createBitmap(source, 0, 0, source.getWidth(), source.getHeight(), transform, true);
                            ByteArrayOutputStream stream = new ByteArrayOutputStream();
                            if (!upright.compress(Bitmap.CompressFormat.JPEG, quality, stream)) {
                                throw new IllegalStateException("Cannot encode the captured JPEG.");
                            }
                            encoded = stream.toByteArray();
                        } catch (Exception | OutOfMemoryError error) {
                            failure = error.toString();
                        } finally {
                            image.close();
                            if (upright != null && upright != source) upright.recycle();
                            if (source != null) source.recycle();
                        }
                        finishPhoto(token, result, encoded, failure);
                    }

                    @Override public void onError(@NonNull ImageCaptureException error) {
                        finishPhoto(token, result, null, error.toString());
                    }
                });
            } catch (RuntimeException error) {
                finishPhoto(token, result, null, error.toString());
            }
        }

        private void finishPhoto(long token, HuxerUIPlatformChannel.Result result, byte[] jpeg, String failure) {
            main.post(() -> {
                photoPending = false;
                if (!active || token != generation) {
                    result.fail("interrupted", "Photo capture was interrupted.", PlatformPayload.nullValue());
                } else if (failure != null) {
                    result.fail("capture-failed", failure, PlatformPayload.nullValue());
                } else {
                    result.complete(PlatformPayload.bytes(jpeg));
                }
                completeStops();
            });
        }

        private void stop() {
            active = false;
            opened = false;
            supportsPointControl = false;
            ++generation;
            if (pendingZoom != null) {
                Choreographer.getInstance().removeFrameCallback(zoomFrame);
                HuxerUIPlatformChannel.Result pending = pendingZoom;
                pendingZoom = null;
                pending.fail("interrupted", "Camera zoom was interrupted.", PlatformPayload.nullValue());
            }
            current = null;
            displays.unregisterDisplayListener(this);
            if (camera != null && cameraObserver != null) {
                camera.getCameraInfo().getCameraState().removeObserver(cameraObserver);
            }
            if (camera != null && torchObserver != null) {
                camera.getCameraInfo().getTorchState().removeObserver(torchObserver);
                camera.getCameraControl().enableTorch(false);
            }
            torchObserver = null;
            cameraObserver = null;
            camera = null;
            lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_STOP);
            if (rateSession != null) {
                preview.setSurfaceProvider(null);
                provider.unbind(rateSession);
                rateSession = null;
                preview = null;
                photoOutput = null;
            }
            if (preview != null) {
                preview.setSurfaceProvider(null);
                provider.unbind(preview);
                preview = null;
            }
            if (photoOutput != null) {
                provider.unbind(photoOutput);
                photoOutput = null;
            }
            for (Output output : new ArrayList<>(outputs)) {
                if (output.gpu != null) output.gpu.pause();
                if (!output.provided) {
                    output.request.willNotProvideSurface();
                    output.release();
                }
            }
            completeStops();
        }

        private void completeStops() {
            if (active || photoPending || !outputs.isEmpty() || frameThreadStopping) return;
            if (frameThread != null) {
                frameThreadStopping = true;
                frameThread.close(() -> main.post(() -> {
                    frameThreadStopping = false;
                    completeStops();
                }));
                frameThread = null;
                return;
            }
            for (HuxerUIPlatformChannel.Result result : stops) result.complete(PlatformPayload.nullValue());
            stops.clear();
            if (disposed) {
                lifecycle.handleLifecycleEvent(Lifecycle.Event.ON_DESTROY);
                photoExecutor.shutdown();
            }
        }

        @Override
        public void dispose() {
            if (disposed) return;
            disposed = true;
            stop();
            events.close();
        }

        private void fail(String code, String message) {
            if (!active || disposed) return;
            Log.e("HuxerUICamera", code + ": " + message);
            Map<String, PlatformPayload> error = new HashMap<>();
            error.put("code", PlatformPayload.string(code));
            error.put("message", PlatformPayload.string(message));
            emit(PlatformPayload.object(error));
            stop();
        }

        private void emit(PlatformPayload error) {
            if (!active || disposed) return;
            Map<String, PlatformPayload> snapshot = new HashMap<>();
            snapshot.put("run", PlatformPayload.int64(run));
            snapshot.put("facing", facing == null ? PlatformPayload.nullValue() : PlatformPayload.string(facing));
            snapshot.put("error", error == null ? PlatformPayload.nullValue() : error);
            snapshot.put("preview", current == null || current.output == null ? PlatformPayload.nullValue() : current.output);
            snapshot.put("captureFormat", current == null || current.output == null
                    ? PlatformPayload.nullValue() : current.captureFormat());
            snapshot.put("canFocusAndMeter", PlatformPayload.booleanValue(supportsPointControl && current != null
                    && !current.invalidated && current.output != null));
            events.emit("snapshot", PlatformPayload.object(snapshot));
        }

        private int displayRotation() {
            WindowManager windows = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            return windows.getDefaultDisplay().getRotation();
        }

        @Override public void onDisplayAdded(int displayId) {}
        @Override public void onDisplayRemoved(int displayId) {}
        @Override public void onDisplayChanged(int displayId) {
            if (!active || preview == null || preview.getTargetRotation() == displayRotation()) return;
            preview.setTargetRotation(displayRotation());
        }

        private final class Output {
            final SurfaceRequest request;
            final long token;
            CameraSurface gpu;
            PlatformPayload output;
            int rotation;
            boolean mirrored;
            boolean swapped;
            boolean provided;
            boolean invalidated;
            boolean released;

            Output(SurfaceRequest request, long token) {
                this.request = request;
                this.token = token;
            }

            PlatformPayload captureFormat() {
                Size size = request.getResolution();
                Range<Integer> range = rateSession == null ? null : rateSession.getFrameRateRange();
                Map<String, PlatformPayload> format = new HashMap<>();
                format.put("width", PlatformPayload.int64(size.getWidth()));
                format.put("height", PlatformPayload.int64(size.getHeight()));
                format.put("minFps", range == null ? PlatformPayload.nullValue() : PlatformPayload.doubleValue(range.getLower()));
                format.put("maxFps", range == null ? PlatformPayload.nullValue() : PlatformPayload.doubleValue(range.getUpper()));
                return PlatformPayload.object(format);
            }

            void configure(SurfaceRequest.TransformationInfo info) {
                if (released || invalidated || !active || token != generation) return;
                int width = request.getResolution().getWidth();
                int height = request.getResolution().getHeight();
                if (!info.getCropRect().equals(new Rect(0, 0, width, height))) {
                    request.willNotProvideSurface();
                    fail("configuration-failed", "CameraX returned a cropped Surface without a ViewPort.");
                    return;
                }
                // The SurfaceTexture transform produces natural
                // orientation; the inverse display rotation remains. See the device validation notes.
                int nextRotation = info.hasCameraTransform()
                        ? (360 - preview.getTargetRotation() * 90) % 360 : info.getRotationDegrees();
                boolean nextSwap = info.hasCameraTransform()
                        && camera.getCameraInfo().getSensorRotationDegrees(Surface.ROTATION_0) % 180 != 0;
                boolean nextMirror = info.hasCameraTransform() && "front".equals(facing);
                if (gpu != null) {
                    if (rotation != nextRotation || swapped != nextSwap || mirrored != nextMirror) {
                        invalidated = true;
                        gpu.pause();
                        request.invalidate();
                        if (!provided) {
                            request.willNotProvideSurface();
                            release();
                        }
                    }
                    return;
                }
                rotation = nextRotation;
                swapped = nextSwap;
                mirrored = nextMirror;
                if (frameThread == null) frameThread = new CameraSurface.FrameThread();
                gpu = new CameraSurface(frameThread, frames, width, height, swapped, rotation, mirrored,
                        usesRealtimeTimestamps(camera.getCameraInfo()));
                gpu.open(surface -> main.post(() -> provide(surface)),
                        frame -> main.post(() -> publish(frame)),
                        error -> main.post(() -> surfaceFailed(error)));
            }

            void provide(Surface surface) {
                if (released || invalidated || !active || token != generation) return;
                try {
                    // CameraX may still write to the surface after invalidation. Its result callback releases it.
                    request.provideSurface(surface, executor, result -> {
                        release();
                        if (active && token == generation && !invalidated
                                && result.getResultCode() != SurfaceRequest.Result.RESULT_SURFACE_USED_SUCCESSFULLY
                                && result.getResultCode() != SurfaceRequest.Result.RESULT_REQUEST_CANCELLED) {
                            fail("configuration-failed", "CameraX rejected the preview Surface: " + result.getResultCode());
                        }
                    });
                    provided = true;
                } catch (RuntimeException error) {
                    surfaceFailed(error);
                    return;
                }
                if (current != null && current != this) current.gpu.pause();
                current = this;
                if (opened) emit(null);
            }

            void publish(PlatformPayload frame) {
                if (released || invalidated || !active || token != generation || current != this) {
                    frame.requireField("texture").requireExternalTexture().close();
                    return;
                }
                clearOutput();
                output = frame;
                if (opened) emit(null);
            }

            void surfaceFailed(RuntimeException error) {
                if (released || invalidated || !active || token != generation) return;
                if (!provided) {
                    request.willNotProvideSurface();
                    release();
                }
                fail(provided ? "capture-failed" : "configuration-failed", error.toString());
            }

            void clearOutput() {
                if (output != null) {
                    output.requireField("texture").requireExternalTexture().close();
                    output = null;
                }
            }

            void release() {
                if (released) return;
                released = true;
                request.clearTransformationInfoListener();
                // A provided Surface reaches this path only after CameraX releases it.
                clearOutput();
                if (current == this) {
                    current = null;
                    if (opened && active && token == generation) emit(null);
                }
                if (gpu != null) gpu.close(() -> main.post(this::finishRelease));
                else finishRelease();
            }

            void finishRelease() {
                outputs.remove(this);
                completeStops();
            }
        }
    }
}
