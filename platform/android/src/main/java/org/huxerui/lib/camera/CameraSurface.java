package org.huxerui.lib.camera;

import android.graphics.SurfaceTexture;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import org.huxerui.PlatformPayload;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

final class CameraSurface {
    interface Callback<T> {
        void accept(T value);
    }

    static final class FrameThread {
        private final HandlerThread thread = new HandlerThread("HuxerUI Camera Frames");
        private final Handler handler;
        private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
        private EGLContext context = EGL14.EGL_NO_CONTEXT;
        private EGLSurface pbuffer = EGL14.EGL_NO_SURFACE;

        FrameThread() {
            thread.start();
            handler = new Handler(thread.getLooper());
        }

        void makeCurrent() {
            if (context == EGL14.EGL_NO_CONTEXT) {
                display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                int[] version = new int[2];
                if (!EGL14.eglInitialize(display, version, 0, version, 1)) {
                    throw new IllegalStateException("Cannot initialize EGL for camera frames.");
                }
                int[] attributes = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                        EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_RED_SIZE, 8,
                        EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_NONE};
                EGLConfig[] configs = new EGLConfig[1];
                int[] count = new int[1];
                if (!EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) || count[0] == 0) {
                    throw new IllegalStateException("Cannot choose an EGL configuration for camera frames.");
                }
                pbuffer = EGL14.eglCreatePbufferSurface(display, configs[0],
                        new int[]{EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
                if (pbuffer == EGL14.EGL_NO_SURFACE) throw new IllegalStateException("Cannot create a camera EGL surface.");
                context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                        new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
                if (context == EGL14.EGL_NO_CONTEXT) {
                    EGL14.eglDestroySurface(display, pbuffer);
                    pbuffer = EGL14.EGL_NO_SURFACE;
                    throw new IllegalStateException("Cannot create a camera EGL context.");
                }
            }
            if (!EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)) {
                throw new IllegalStateException("Cannot bind the camera EGL context.");
            }
        }

        void close(Runnable completed) {
            handler.post(() -> {
                if (context != EGL14.EGL_NO_CONTEXT) {
                    EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                    EGL14.eglDestroySurface(display, pbuffer);
                    EGL14.eglDestroyContext(display, context);
                }
                EGL14.eglReleaseThread();
                thread.quitSafely();
                completed.run();
            });
        }
    }

    private static final String VERTEX = "attribute vec2 position; uniform mat4 transform; varying vec2 uv;"
            + "void main() { gl_Position = vec4(position, 0., 1.);"
            + "uv = (transform * vec4(position * .5 + .5, 0., 1.)).xy; }";
    private static final String FRAGMENT = "#extension GL_OES_EGL_image_external : require\n"
            + "precision mediump float; uniform samplerExternalOES image; varying vec2 uv;"
            + "void main() { gl_FragColor = vec4(texture2D(image, uv).rgb, 1.); }";

    private final FrameThread worker;
    private final int sourceWidth;
    private final int sourceHeight;
    private final int width;
    private final int height;
    private final int rotation;
    private final boolean mirrored;
    private final long frames;
    private final FloatBuffer vertices = ByteBuffer.allocateDirect(8 * Float.BYTES)
            .order(ByteOrder.nativeOrder()).asFloatBuffer();
    private final float[] transform = new float[16];
    private final int[] textures = new int[2];
    private int framebuffer;
    private int program;
    private int position;
    private int transformUniform;
    private int imageUniform;
    private SurfaceTexture source;
    private Surface surface;
    private long output;
    private long lastTimestamp = Long.MIN_VALUE;
    private volatile boolean paused;

    CameraSurface(FrameThread worker, long frames, int width, int height, boolean swapped, int rotation, boolean mirrored) {
        this.worker = worker;
        this.frames = frames;
        sourceWidth = width;
        sourceHeight = height;
        this.width = swapped ? height : width;
        this.height = swapped ? width : height;
        this.rotation = rotation;
        this.mirrored = mirrored;
        vertices.put(new float[]{-1, -1, 1, -1, -1, 1, 1, 1}).position(0);
    }

    void open(Callback<Surface> ready, Callback<PlatformPayload> published, Callback<RuntimeException> failed) {
        worker.handler.post(() -> {
            if (paused) return;
            try {
                setup();
                source.setOnFrameAvailableListener(ignored -> {
                    if (paused) return;
                    try {
                        PlatformPayload frame = render();
                        if (frame != null) published.accept(frame);
                    } catch (RuntimeException error) {
                        pause();
                        failed.accept(error);
                    }
                }, worker.handler);
                ready.accept(surface);
            } catch (RuntimeException error) {
                failed.accept(error);
            }
        });
    }

    void pause() { paused = true; }

    void close(Runnable completed) {
        pause();
        worker.handler.post(() -> {
            try {
                if (output != 0) finishOutput(output);
                if (surface != null) surface.release();
                if (source != null) source.release();
                if (program != 0 || textures[0] != 0 || framebuffer != 0) {
                    worker.makeCurrent();
                    GLES20.glDeleteFramebuffers(1, new int[]{framebuffer}, 0);
                    GLES20.glDeleteTextures(2, textures, 0);
                    GLES20.glDeleteProgram(program);
                }
            } catch (RuntimeException error) {
                Log.w("HuxerUICamera", "Could not release the camera frame resources.", error);
            } finally {
                completed.run();
            }
        });
    }

    private void setup() {
        worker.makeCurrent();
        program = createProgram();
        position = GLES20.glGetAttribLocation(program, "position");
        transformUniform = GLES20.glGetUniformLocation(program, "transform");
        imageUniform = GLES20.glGetUniformLocation(program, "image");
        GLES20.glGenTextures(2, textures, 0);
        bindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textures[0]);
        bindTexture(GLES20.GL_TEXTURE_2D, textures[1]);
        int[] framebuffers = new int[1];
        GLES20.glGenFramebuffers(1, framebuffers, 0);
        framebuffer = framebuffers[0];
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, framebuffer);
        GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0,
                GLES20.GL_TEXTURE_2D, textures[1], 0);
        source = new SurfaceTexture(textures[0]);
        source.setDefaultBufferSize(sourceWidth, sourceHeight);
        surface = new Surface(source);
        output = createOutput(frames, width, height, rotation, mirrored);
    }

    private PlatformPayload render() {
        worker.makeCurrent();
        source.updateTexImage();
        // Read the timestamp of the image latched by updateTexImage.
        long timestamp = source.getTimestamp();
        if (timestamp == lastTimestamp) return null;
        lastTimestamp = timestamp;
        source.getTransformMatrix(transform);
        // Redefine the image storage to detach it from the previous frame's EGLImage.
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textures[1]);
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, width, height, 0,
                GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, 0);
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, framebuffer);
        if (GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER) != GLES20.GL_FRAMEBUFFER_COMPLETE) {
            throw new IllegalStateException("Cannot allocate the camera frame texture.");
        }
        GLES20.glViewport(0, 0, width, height);
        GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
        GLES20.glDisable(GLES20.GL_BLEND);
        GLES20.glDisable(GLES20.GL_DEPTH_TEST);
        GLES20.glDisable(GLES20.GL_CULL_FACE);
        GLES20.glColorMask(true, true, true, true);
        GLES20.glUseProgram(program);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textures[0]);
        GLES20.glUniform1i(imageUniform, 0);
        GLES20.glUniformMatrix4fv(transformUniform, 1, false, transform, 0);
        GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, 0);
        GLES20.glVertexAttribPointer(position, 2, GLES20.GL_FLOAT, false, 0, vertices);
        GLES20.glEnableVertexAttribArray(position);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        GLES20.glDisableVertexAttribArray(position);
        if (GLES20.glGetError() != GLES20.GL_NO_ERROR) throw new IllegalStateException("Cannot draw the camera frame.");
        // JNI publishes and processes the frame while its capture EGL context is current.
        return processFrame(output, textures[1], timestamp);
    }

    private static void bindTexture(int target, int texture) {
        GLES20.glBindTexture(target, texture);
        GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(target, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
    }

    private static int compileShader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        int[] status = new int[1];
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0);
        if (status[0] != GLES20.GL_TRUE) {
            String error = GLES20.glGetShaderInfoLog(shader);
            GLES20.glDeleteShader(shader);
            throw new IllegalStateException("Cannot compile the camera frame shader: " + error);
        }
        return shader;
    }

    private static int createProgram() {
        int vertex = compileShader(GLES20.GL_VERTEX_SHADER, VERTEX);
        int fragment = 0;
        try {
            fragment = compileShader(GLES20.GL_FRAGMENT_SHADER, FRAGMENT);
            int program = GLES20.glCreateProgram();
            GLES20.glAttachShader(program, vertex);
            GLES20.glAttachShader(program, fragment);
            GLES20.glLinkProgram(program);
            int[] status = new int[1];
            GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, status, 0);
            if (status[0] != GLES20.GL_TRUE) {
                String error = GLES20.glGetProgramInfoLog(program);
                GLES20.glDeleteProgram(program);
                throw new IllegalStateException("Cannot link the camera frame shader: " + error);
            }
            return program;
        } finally {
            GLES20.glDeleteShader(vertex);
            if (fragment != 0) GLES20.glDeleteShader(fragment);
        }
    }

    private static native long createOutput(long frames, int width, int height, int rotation, boolean mirrored);
    private static native PlatformPayload processFrame(long output, int texture, long timestamp);
    private static native void finishOutput(long output);
}
