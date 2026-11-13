#include "camera_internal.h"

#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <wrl/client.h>

#include <stdexcept>

#include <huxerui/windows/external_texture.h>

namespace huxerui::camera::detail {
namespace {

using Microsoft::WRL::ComPtr;

void Require(HRESULT result, const char* message) {
  if (FAILED(result)) throw CameraError{CameraErrorCode::ConfigurationFailed, message};
}

} // namespace

struct TextureResizer::State {
  ComPtr<ID3D11Device> device;
  ComPtr<ID2D1DeviceContext> draw;
  ComPtr<ID3D11Texture2D> source;
  ComPtr<ID2D1Bitmap1> source_bitmap;
  ComPtr<ID3D11Texture2D> target;
  ComPtr<ID2D1Bitmap1> target_bitmap;
  std::shared_ptr<windows::D3D11Texture> texture;

  ~State() {
    if (texture) texture->Finish();
  }

  void Initialize(ID3D11DeviceContext& context, UINT width, UINT height) {
    context.GetDevice(device.GetAddressOf());
    ComPtr<IDXGIDevice> dxgi;
    Require(device.As(&dxgi), "Cannot access the camera output DXGI device.");
    ComPtr<ID2D1Factory1> factory;
    Require(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), nullptr,
                reinterpret_cast<void**>(factory.GetAddressOf())),
            "Cannot create a Direct2D factory for camera output.");
    ComPtr<ID2D1Device> drawing_device;
    Require(factory->CreateDevice(dxgi.Get(), drawing_device.GetAddressOf()),
            "Cannot create a Direct2D device for camera output.");
    Require(drawing_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, draw.GetAddressOf()),
            "Cannot create a Direct2D context for camera output.");
    const auto limit = draw->GetMaximumBitmapSize();
    if (width > limit || height > limit) {
      throw CameraError{CameraErrorCode::ConfigurationFailed, "The requested camera output exceeds Direct2D limits."};
    }
    draw->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    target = CreateTexture(width, height, D3D11_BIND_RENDER_TARGET);
    target_bitmap = CreateBitmap(*target.Get(), D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1_ALPHA_MODE_PREMULTIPLIED);
    texture = std::make_shared<windows::D3D11Texture>(Size{static_cast<float>(width), static_cast<float>(height)});
  }

  void SetSource(UINT width, UINT height, D2D1_ALPHA_MODE alpha) {
    if (source_bitmap) {
      const auto size = source_bitmap->GetPixelSize();
      if (size.width == width && size.height == height && source_bitmap->GetPixelFormat().alphaMode == alpha) return;
    }
    source = CreateTexture(width, height, D3D11_BIND_SHADER_RESOURCE);
    source_bitmap = CreateBitmap(*source.Get(), D2D1_BITMAP_OPTIONS_NONE, alpha);
  }

  ComPtr<ID3D11Texture2D> CreateTexture(UINT width, UINT height, UINT bind_flags) const {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bind_flags;
    ComPtr<ID3D11Texture2D> result;
    Require(device->CreateTexture2D(&description, nullptr, result.GetAddressOf()),
            "Cannot allocate a D3D11 texture for camera output.");
    return result;
  }

  ComPtr<ID2D1Bitmap1> CreateBitmap(ID3D11Texture2D& texture, D2D1_BITMAP_OPTIONS options, D2D1_ALPHA_MODE alpha) const {
    ComPtr<IDXGISurface> surface;
    Require(texture.QueryInterface(IID_PPV_ARGS(surface.GetAddressOf())), "Cannot access the camera output surface.");
    const auto properties = D2D1::BitmapProperties1(options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alpha),
                                                    96.0F, 96.0F);
    ComPtr<ID2D1Bitmap1> result;
    Require(draw->CreateBitmapFromDxgiSurface(surface.Get(), &properties, result.GetAddressOf()),
            "Cannot create a Direct2D bitmap for camera output.");
    return result;
  }
};

TextureResizer::TextureResizer() = default;
TextureResizer::~TextureResizer() = default;
void TextureResizer::Reset() { state_.reset(); }

std::optional<Rect> TextureResizer::Resize(PreviewOutput& frame, std::uint32_t width, std::uint32_t height) {
  const auto* source = dynamic_cast<const windows::D3D11Texture*>(frame.texture.get());
  const auto snapshot = source ? source->AcquireFrame() : nullptr;
  if (!snapshot) {
    throw CameraError{CameraErrorCode::ConfigurationFailed, "Expected a published D3D11Texture for camera output sizing."};
  }
  const auto source_width = static_cast<UINT>(snapshot->PixelWidth());
  const auto source_height = static_cast<UINT>(snapshot->PixelHeight());
  if (width == source_width && height == source_height && frame.crop == full_texture_crop) return full_texture_crop;
  const auto crop = CropForOutput(frame.crop, source_width, source_height, width, height);

  if (!state_) {
    if (!context_) throw std::logic_error("The camera output requires a D3D11 device context.");
    state_ = std::make_unique<State>();
    state_->Initialize(*context_, width, height);
  }
  auto& state = *state_;
  state.SetSource(source_width, source_height, snapshot->Alpha() == windows::D3D11Texture::Alpha::Opaque
      ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED);
  if (!snapshot->TryCopyTo(*context_, *state.source.Get())) return {};

  const auto from = D2D1::RectF(crop.x * source_width, crop.y * source_height,
                               (crop.x + crop.width) * source_width, (crop.y + crop.height) * source_height);
  const auto to = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
  state.draw->SetTarget(state.target_bitmap.Get());
  state.draw->BeginDraw();
  state.draw->Clear(D2D1::ColorF(0, 0, 0, 0));
  state.draw->DrawBitmap(state.source_bitmap.Get(), &to, 1.0F, D2D1_INTERPOLATION_MODE_LINEAR, &from, nullptr);
  const auto result = state.draw->EndDraw();
  state.draw->SetTarget(nullptr);
  if (FAILED(result)) throw std::runtime_error("Cannot scale the camera output bitmap.");

  state.texture->Publish({.texture = state.target.Get(), .alpha = snapshot->Alpha()});
  frame.texture = state.texture;
  frame.crop = full_texture_crop;
  return crop;
}

} // namespace huxerui::camera::detail
