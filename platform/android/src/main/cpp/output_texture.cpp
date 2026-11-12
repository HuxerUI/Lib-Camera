#include "camera_internal.h"

#include <algorithm>
#include <stdexcept>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <huxerui/android/external_texture.h>

namespace huxerui::camera::detail {
namespace {

constexpr char vertex_source[] = R"(
attribute vec2 position;
uniform vec4 crop;
varying mediump vec2 uv;
void main() {
  gl_Position = vec4(position, 0.0, 1.0);
  uv = crop.xy + (position * 0.5 + 0.5) * crop.zw;
}
)";

constexpr char fragment_source[] = R"(
precision mediump float;
uniform sampler2D image;
varying mediump vec2 uv;
void main() {
  gl_FragColor = texture2D(image, uv);
}
)";

GLuint CompileShader(GLenum type, const char* source) {
  const auto shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) return shader;
  glDeleteShader(shader);
  throw std::runtime_error("Cannot compile the camera output shader.");
}

GLuint CreateProgram() {
  const auto vertex = CompileShader(GL_VERTEX_SHADER, vertex_source);
  GLuint fragment;
  try {
    fragment = CompileShader(GL_FRAGMENT_SHADER, fragment_source);
  } catch (...) {
    glDeleteShader(vertex);
    throw;
  }
  const auto program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  glBindAttribLocation(program, 0, "position");
  glLinkProgram(program);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) return program;
  glDeleteProgram(program);
  throw std::runtime_error("Cannot link the camera output shader.");
}

void SetSampling() {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

struct ImportedTexture {
  const GLuint name;
  ~ImportedTexture() { glDeleteTextures(1, &name); }
};

} // namespace

bool SupportsGlOutputSize(std::uint32_t width, std::uint32_t height) {
  GLint limit = 0;
  GLint viewport[2]{};
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
  glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport);
  return width <= static_cast<std::uint32_t>(std::min(limit, viewport[0])) &&
         height <= static_cast<std::uint32_t>(std::min(limit, viewport[1]));
}

struct TextureResizer::State {
  EGLContext context = eglGetCurrentContext();
  GLuint program = 0;
  GLuint target = 0;
  GLuint framebuffer = 0;
  GLint crop_uniform = -1;
  std::shared_ptr<android::GlTexture> texture;

  ~State() {
    if (texture) texture->Finish();
    if (context != EGL_NO_CONTEXT && eglGetCurrentContext() == context) {
      glDeleteFramebuffers(1, &framebuffer);
      glDeleteTextures(1, &target);
      glDeleteProgram(program);
    }
  }

  void Initialize(std::uint32_t width, std::uint32_t height) {
    if (!SupportsGlOutputSize(width, height)) {
      throw CameraError{CameraErrorCode::ConfigurationFailed, "The requested camera output exceeds OpenGL ES limits."};
    }
    program = CreateProgram();
    crop_uniform = glGetUniformLocation(program, "crop");
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "image"), 0);
    glGenTextures(1, &target);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, target);
    SetSampling();
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    texture = std::make_shared<android::GlTexture>(Size{static_cast<float>(width), static_cast<float>(height)});
  }
};

TextureResizer::TextureResizer() = default;
TextureResizer::~TextureResizer() = default;
void TextureResizer::Reset() { state_.reset(); }

bool TextureResizer::Resize(PreviewOutput& frame, std::uint32_t width, std::uint32_t height) {
  const auto* source = dynamic_cast<const android::GlTexture*>(frame.texture.get());
  const auto snapshot = source ? source->AcquireFrame() : nullptr;
  if (!snapshot) {
    throw CameraError{CameraErrorCode::ConfigurationFailed, "Expected a published GlTexture for camera output sizing."};
  }
  const auto source_width = static_cast<std::uint32_t>(snapshot->PixelWidth());
  const auto source_height = static_cast<std::uint32_t>(snapshot->PixelHeight());
  if (width == source_width && height == source_height && frame.crop == full_texture_crop) return true;
  const auto crop = CropForOutput(frame.crop, source_width, source_height, width, height);

  if (!state_) {
    state_ = std::make_unique<State>();
    state_->Initialize(width, height);
  }
  const auto& state = *state_;
  const auto pixel_width = static_cast<GLsizei>(width);
  const auto pixel_height = static_cast<GLsizei>(height);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, state.target);
  // Replace the storage before publishing it again as a producer EGLImage.
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixel_width, pixel_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindFramebuffer(GL_FRAMEBUFFER, state.framebuffer);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    throw CameraError{CameraErrorCode::ConfigurationFailed, "Cannot allocate the camera output framebuffer."};
  }
  const ImportedTexture image{snapshot->ImportCurrent()};
  glBindTexture(GL_TEXTURE_2D, image.name);
  SetSampling();
  glViewport(0, 0, pixel_width, pixel_height);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(state.program);
  glUniform4f(state.crop_uniform, crop.x, 1.0F - crop.y - crop.height, crop.width, crop.height);
  constexpr GLfloat vertices[]{-1, -1, 1, -1, -1, 1, 1, 1};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Cannot scale the camera output texture.");

  state.texture->PublishCurrent({.texture_name = state.target, .pixel_width = pixel_width, .pixel_height = pixel_height});
  frame.texture = state.texture;
  frame.crop = full_texture_crop;
  return true;
}

} // namespace huxerui::camera::detail
