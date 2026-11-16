package org.huxerui.lib.camera.preview;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Decodes one camera JPEG into tightly packed RGBA pixels for the C++ QR codec.
 *
 * The payload layout is an unsigned 32-bit big-endian width, an unsigned 32-bit
 * big-endian height, then width * height * 4 bytes in R, G, B, A order.
 */
public final class QrImageDecoderModule implements HuxerUIPlatformModule.Factory {
    private static final int MAX_SCAN_DIMENSION = 1280;

    @Override
    public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                                       HuxerUIPlatformChannel.Events events) {
        return new Decoder();
    }

    private static final class Decoder implements HuxerUIPlatformModule {
        private final ExecutorService executor = Executors.newSingleThreadExecutor();
        private volatile boolean disposed;

        @Override
        public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload arguments,
                                                          HuxerUIPlatformChannel.Result result) {
            if (disposed || !method.equals("decode")) {
                result.fail("qr/unavailable", "QR scanning is unavailable.", PlatformPayload.nullValue());
                return null;
            }
            byte[] jpeg = arguments.requireBytes();
            AtomicBoolean canceled = new AtomicBoolean();
            Future<?> work = executor.submit(() -> {
                Bitmap bitmap = null;
                try {
                    if (canceled.get() || disposed) return;
                    BitmapFactory.Options options = new BitmapFactory.Options();
                    options.inJustDecodeBounds = true;
                    BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length, options);
                    if (options.outWidth <= 0 || options.outHeight <= 0) {
                        throw new IllegalArgumentException("Cannot read the camera image dimensions.");
                    }
                    options.inSampleSize = 1;
                    while ((Math.max(options.outWidth, options.outHeight) + (long) options.inSampleSize - 1)
                            / options.inSampleSize > MAX_SCAN_DIMENSION) {
                        options.inSampleSize *= 2;
                    }
                    options.inJustDecodeBounds = false;
                    options.inScaled = false;
                    options.inPreferredConfig = Bitmap.Config.ARGB_8888;
                    bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.length, options);
                    if (bitmap == null) throw new IllegalArgumentException("Cannot decode the camera image.");
                    if (canceled.get() || disposed) return;
                    final int width = bitmap.getWidth();
                    final int height = bitmap.getHeight();
                    final int[] argb = new int[width * height];
                    bitmap.getPixels(argb, 0, width, 0, 0, width, height);
                    ByteBuffer output = ByteBuffer.allocate(8 + width * height * 4)
                            .order(ByteOrder.BIG_ENDIAN);
                    output.putInt(width);
                    output.putInt(height);
                    for (int pixel : argb) {
                        output.put((byte) (pixel >> 16));
                        output.put((byte) (pixel >> 8));
                        output.put((byte) pixel);
                        output.put((byte) (pixel >>> 24));
                    }
                    if (!canceled.get() && !disposed) {
                        result.complete(PlatformPayload.bytes(output.array()));
                    }
                } catch (RuntimeException | OutOfMemoryError error) {
                    if (!canceled.get() && !disposed) {
                        result.fail("qr/decode-failed", "Cannot decode the camera image for scanning.",
                                PlatformPayload.nullValue());
                    }
                } finally {
                    if (bitmap != null) bitmap.recycle();
                }
            });
            return () -> {
                canceled.set(true);
                work.cancel(false);
            };
        }

        @Override
        public void dispose() {
            disposed = true;
            executor.shutdownNow();
        }
    }
}
