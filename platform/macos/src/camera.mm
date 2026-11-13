#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>

#include <vector>
#include <utility>

#include <huxerui/macos/external_texture.h>

#include "camera_internal.h"

#define CAMERA_UNUSED(name) static_cast<void>(name)

using namespace huxerui;
using namespace huxerui::camera;
using huxerui::camera::detail::CameraBackend;
using huxerui::camera::detail::FramePipeline;
using huxerui::camera::detail::FrameProducer;
using huxerui::camera::detail::ProcessedFrame;
using huxerui::camera::detail::full_texture_crop;

namespace {

char lighting_observation;

CMClockRef SessionClock(AVCaptureSession* session) {
  if (@available(macOS 12.3, *)) return session.synchronizationClock;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return session.masterClock;
#pragma clang diagnostic pop
}

CameraResult<ImageAsset> EncodePhoto(NSData* data, PhotoOptions options, CIContext* context) {
  @autoreleasepool {
    CIImage* image = [CIImage imageWithData:data options:@{kCIImageApplyOrientationProperty : @YES}];
    if (!image || CGRectIsEmpty(image.extent) || CGRectIsInfinite(image.extent)) {
      return CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Cannot decode the captured photo."});
    }
    if (options.mirror == MirrorMode::On) {
      image = [image imageByApplyingTransform:CGAffineTransformMakeScale(-1, 1)];
    }
    image = [image
        imageByApplyingTransform:CGAffineTransformMakeTranslation(-image.extent.origin.x, -image.extent.origin.y)];
    CGImageRef pixels = [context createCGImage:image fromRect:image.extent];
    if (!pixels)
      return CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::CaptureFailed, "Cannot normalize the captured photo."});
    NSMutableData* jpeg = [NSMutableData new];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData((__bridge CFMutableDataRef)jpeg, CFSTR("public.jpeg"), 1, nullptr);
    bool encoded = false;
    if (destination) {
      NSDictionary* properties = @{
        (id)kCGImageDestinationLossyCompressionQuality : @(options.jpeg_quality / 100.0),
        (id)kCGImagePropertyOrientation : @1
      };
      CGImageDestinationAddImage(destination, pixels, (__bridge CFDictionaryRef)properties);
      encoded = CGImageDestinationFinalize(destination);
      CFRelease(destination);
    }
    CGImageRelease(pixels);
    if (!encoded)
      return CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Cannot encode the captured JPEG."});
    const auto* begin = static_cast<const std::byte*>(jpeg.bytes);
    return CameraResult<ImageAsset>::Success(ImageAsset::FromEncoded(Bytes(begin, begin + jpeg.length)));
  }
}

} // namespace

@interface HUXMacPhotoCapture : NSObject <AVCapturePhotoCaptureDelegate> {
  NSData* data_;
  NSError* processingError_;
  PhotoOptions options_;
  dispatch_queue_t processingQueue_;
  CIContext* context_;
  std::function<void(CameraResult<ImageAsset>)> completed_;
}
- (instancetype)initWithOptions:(PhotoOptions)options
                          queue:(dispatch_queue_t)queue
                        context:(CIContext*)context
                      completed:(std::function<void(CameraResult<ImageAsset>)>)completed;
@end

@implementation HUXMacPhotoCapture

- (instancetype)initWithOptions:(PhotoOptions)options
                          queue:(dispatch_queue_t)queue
                        context:(CIContext*)context
                      completed:(std::function<void(CameraResult<ImageAsset>)>)completed {
  self = [super init];
  if (self) {
    options_ = options;
    processingQueue_ = queue;
    context_ = context;
    completed_ = std::move(completed);
  }
  return self;
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishProcessingPhoto:(AVCapturePhoto*)photo
                       error:(NSError*)error {
  CAMERA_UNUSED(output);
  processingError_ = error;
  data_ = error ? nil : photo.fileDataRepresentation;
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishCaptureForResolvedSettings:(AVCaptureResolvedPhotoSettings*)settings
                                  error:(NSError*)error {
  CAMERA_UNUSED(output);
  CAMERA_UNUSED(settings);
  NSError* failure = error ?: processingError_;
  dispatch_async(processingQueue_, ^{
    auto completed = std::exchange(completed_, {});
    if (!completed)
      return;
    if (failure || !data_) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::CaptureFailed,
           failure.localizedDescription.UTF8String ?: "The camera returned no photo data."}));
      return;
    }
    try {
      @try {
        completed(EncodePhoto(data_, options_, context_));
      } @catch (NSException* exception) {
        completed(CameraResult<ImageAsset>::Failure(
            {CameraErrorCode::CaptureFailed, exception.reason.UTF8String ?: "Photo encoding failed."}));
      }
    } catch (const std::exception& exception) {
      completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, exception.what()}));
    }
  });
}

@end

@interface HUXCameraCapture : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
  dispatch_queue_t queue_;
  AVCaptureSession* session_;
  AVCaptureVideoDataOutput* output_;
  AVCapturePhotoOutput* photoOutput_;
  HUXMacPhotoCapture* photo_;
  dispatch_queue_t photoQueue_;
  CIContext* photoContext_;
  std::vector<std::function<void()>> stops_;
  std::uint64_t photoGeneration_;
  AVCaptureDevice* device_;
  NSMutableArray* observers_;
  std::shared_ptr<macos::PixelBufferTexture> texture_;
  std::shared_ptr<FramePipeline> frames_;
  FrameProducer producer_;
  std::optional<ProcessedFrame> preview_;
  std::optional<Facing> facing_;
  bool failed_;
  CameraBackend::StatusCallback changed_;
  std::function<void(PhotoFlash, std::optional<Torch>)> lightingChanged_;
}
- (void)start:(std::optional<Facing>)facing frames:(std::shared_ptr<FramePipeline>)frames
      changed:(CameraBackend::StatusCallback)changed;
- (void)stop:(std::function<void()>)completed;
- (void)observeLighting:(std::function<void(PhotoFlash, std::optional<Torch>)>)changed;
- (void)setTorchEnabled:(bool)enabled completed:(std::function<void(CameraResult<void>)>)completed;
- (void)turnOffTorch;
- (void)capturePhoto:(PhotoOptions)options completed:(std::function<void(CameraResult<ImageAsset>)>)completed;
- (bool)supportsPointControl;
- (void)focusAndMeter:(huxerui::Point)point completed:(std::function<void(CameraResult<void>)>)completed;
@end

@implementation HUXCameraCapture

- (instancetype)init {
  self = [super init];
  if (self) {
    queue_ = dispatch_queue_create("org.huxerui.camera.capture", DISPATCH_QUEUE_SERIAL);
    observers_ = [NSMutableArray new];
    photoQueue_ = dispatch_queue_create("org.huxerui.camera.macos.photo", DISPATCH_QUEUE_SERIAL);
  }
  return self;
}

- (void)fail:(CameraErrorCode)code message:(NSString*)message {
  if (changed_ && !failed_) {
    failed_ = true;
    changed_({.state = SessionState::Failed,
              .error = CameraError{code, message.UTF8String ?: "Camera capture failed."}}, full_texture_crop);
  }
}

- (void)start:(std::optional<Facing>)facing frames:(std::shared_ptr<FramePipeline>)frames
      changed:(CameraBackend::StatusCallback)changed {
  dispatch_async(queue_, ^{
    changed_ = changed;
    frames_ = frames;
    failed_ = false;
    if (!facing) {
      device_ = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    } else {
      const AVCaptureDevicePosition position = *facing == Facing::Front ? AVCaptureDevicePositionFront :
                                                                         AVCaptureDevicePositionBack;
      AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
                                mediaType:AVMediaTypeVideo position:position];
      for (AVCaptureDevice* candidate in discovery.devices) {
        if (candidate.position == position) {
          device_ = candidate;
          break;
        }
      }
    }
    if (!device_) {
      [self fail:CameraErrorCode::DeviceNotFound message:@"No camera matches the requested facing."];
      return;
    }
    facing_.reset();
    if (device_.position == AVCaptureDevicePositionFront) {
      facing_ = Facing::Front;
    } else if (device_.position == AVCaptureDevicePositionBack) {
      facing_ = Facing::Back;
    }
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device_ error:&error];
    if (!input) {
      const auto code = error.code == AVErrorDeviceInUseByAnotherApplication ? CameraErrorCode::DeviceBusy :
                                                                             CameraErrorCode::ConfigurationFailed;
      [self fail:code message:error.localizedDescription];
      return;
    }
    session_ = [AVCaptureSession new];
    [session_ beginConfiguration];
    if ([session_ canSetSessionPreset:AVCaptureSessionPreset1280x720]) {
      session_.sessionPreset = AVCaptureSessionPreset1280x720;
    }
    output_ = [AVCaptureVideoDataOutput new];
    output_.alwaysDiscardsLateVideoFrames = YES;
    output_.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
    if (![session_ canAddInput:input]) {
      [session_ commitConfiguration];
      [self fail:CameraErrorCode::ConfigurationFailed message:@"Cannot attach the camera input."];
      return;
    }
    [session_ addInput:input];
    if (![session_ canAddOutput:output_]) {
      [session_ commitConfiguration];
      [self fail:CameraErrorCode::ConfigurationFailed message:@"Cannot attach the preview output."];
      return;
    }
    [session_ addOutput:output_];
    AVCaptureConnection* connection = [output_ connectionWithMediaType:AVMediaTypeVideo];
    if (connection.isVideoMirroringSupported) {
      connection.automaticallyAdjustsVideoMirroring = NO;
      connection.videoMirrored = NO;
    }
    photoOutput_ = [AVCapturePhotoOutput new];
    if ([session_ canAddOutput:photoOutput_]) {
      [session_ addOutput:photoOutput_];
      if (![photoOutput_.availablePhotoCodecTypes containsObject:AVVideoCodecTypeJPEG]) {
        [session_ removeOutput:photoOutput_];
        photoOutput_ = nil;
      }
    } else {
      photoOutput_ = nil;
    }
    [output_ setSampleBufferDelegate:self queue:queue_];
    [session_ commitConfiguration];
    __weak HUXCameraCapture* weak = self;
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [observers_ addObject:[center addObserverForName:AVCaptureSessionRuntimeErrorNotification object:session_
                                             queue:nil usingBlock:^(NSNotification* notification) {
      HUXCameraCapture* capture = weak;
      if (!capture) {
        return;
      }
      NSError* runtimeError = notification.userInfo[AVCaptureSessionErrorKey];
      dispatch_async(capture->queue_, ^{
        if (notification.object == capture->session_) {
          [capture fail:CameraErrorCode::CaptureFailed message:runtimeError.localizedDescription];
        }
      });
    }]];
    [observers_ addObject:[center addObserverForName:AVCaptureDeviceWasDisconnectedNotification object:device_
                                             queue:nil usingBlock:^(NSNotification* notification) {
      HUXCameraCapture* capture = weak;
      if (capture) {
        dispatch_async(capture->queue_, ^{
          if (notification.object == capture->device_) {
            [capture fail:CameraErrorCode::DeviceDisconnected message:@"The camera was disconnected."];
          }
        });
      }
    }]];
    [session_ startRunning];
    if (!session_.isRunning) {
      [self fail:CameraErrorCode::CaptureFailed message:@"The camera did not start."];
      return;
    }
    changed_({.state = SessionState::Running, .actual_facing = facing_}, full_texture_crop);
  });
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection*)connection {
  if (output != output_ || !changed_ || failed_) {
    return;
  }
  CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
  if (!buffer) {
    return;
  }
  try {
    const auto width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(buffer));
    const auto height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(buffer));
    const huxerui::Size size{static_cast<float>(width), static_cast<float>(height)};
    if (!texture_ || texture_->IntrinsicSize() != size) {
      producer_.processor = {};
      if (texture_) {
        texture_->Finish();
      }
      texture_ = std::make_shared<macos::PixelBufferTexture>(size);
    }
    texture_->Publish(buffer);
    ProcessedFrame frame{.output = {.texture = texture_, .mirrored = static_cast<bool>(connection.videoMirrored)}};
    if (frames_) {
      const auto time = CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample), 1'000'000'000,
                                          kCMTimeRoundingMethod_Default);
      if (!CMTIME_IS_NUMERIC(time)) return;
      std::optional<std::chrono::nanoseconds> synchronized_time;
      if (const auto clock = SessionClock(session_)) {
        const auto host = CMSyncConvertTime(time, clock, CMClockGetHostTimeClock());
        const auto timestamp = CMTimeConvertScale(host, 1'000'000'000, kCMTimeRoundingMethod_Default);
        if (CMTIME_IS_NUMERIC(timestamp)) synchronized_time = std::chrono::nanoseconds(timestamp.value);
      }
      auto processed = frames_->Process(producer_, std::move(frame.output), std::chrono::nanoseconds(time.value),
                                        width, height, synchronized_time);
      if (!processed.Succeeded()) {
        failed_ = true;
        changed_({.state = SessionState::Failed, .error = processed.Error()}, full_texture_crop);
        return;
      }
      frame = std::move(processed).Value();
    }
    if (frame.output.texture && frame != preview_) {
      preview_ = std::move(frame);
      changed_({.state = SessionState::Running, .actual_facing = facing_, .preview = preview_->output,
                .can_focus_and_meter = [self supportsPointControl]}, preview_->source_crop);
    }
  } catch (const std::exception& error) {
    [self fail:CameraErrorCode::CaptureFailed message:[NSString stringWithUTF8String:error.what()]];
  }
}

- (bool)supportsPointControl {
  return (device_.isFocusPointOfInterestSupported &&
          [device_ isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus]) ||
         (device_.isExposurePointOfInterestSupported &&
          [device_ isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]);
}

- (void)focusAndMeter:(huxerui::Point)point completed:(std::function<void(CameraResult<void>)>)completed {
  dispatch_async(queue_, ^{
    if (!device_ || !changed_ || !session_.isRunning || failed_ || !preview_) {
      completed(CameraResult<void>::Failure({CameraErrorCode::NotReady, "The camera preview is not ready."}));
      return;
    }
    if (![self supportsPointControl]) {
      completed(CameraResult<void>::Failure(
          {CameraErrorCode::Unavailable, "Point focus and metering are unavailable."}));
      return;
    }
    NSError* error = nil;
    if (![device_ lockForConfiguration:&error]) {
      completed(CameraResult<void>::Failure({CameraErrorCode::CaptureFailed,
          error.localizedDescription.UTF8String ?: "Cannot configure camera focus."}));
      return;
    }
    NSString* failure = nil;
    @try {
      const CGPoint target = CGPointMake(preview_->output.mirrored ? 1.0F - point.x : point.x, point.y);
      if (device_.isFocusPointOfInterestSupported &&
          [device_ isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus]) {
        device_.focusPointOfInterest = target;
        device_.focusMode = AVCaptureFocusModeContinuousAutoFocus;
      }
      if (device_.isExposurePointOfInterestSupported &&
          [device_ isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]) {
        device_.exposurePointOfInterest = target;
        device_.exposureMode = AVCaptureExposureModeContinuousAutoExposure;
      }
    } @catch (NSException* exception) {
      failure = exception.reason ?: @"Cannot apply camera focus and metering.";
    } @finally {
      [device_ unlockForConfiguration];
    }
    if (failure) {
      completed(CameraResult<void>::Failure({CameraErrorCode::CaptureFailed, failure.UTF8String}));
    } else {
      completed(CameraResult<void>::Success());
    }
  });
}

- (PhotoFlash)photoFlash {
  if (@available(macOS 13.0, *)) {
    NSArray<NSNumber*>* modes = photoOutput_.supportedFlashModes;
    return {.supports_auto = [modes containsObject:@(AVCaptureFlashModeAuto)] == YES,
            .supports_on = [modes containsObject:@(AVCaptureFlashModeOn)] == YES};
  }
  return {};
}

- (std::optional<Torch>)torch {
  if (!device_.hasTorch || ![device_ isTorchModeSupported:AVCaptureTorchModeOn] ||
      ![device_ isTorchModeSupported:AVCaptureTorchModeOff]) return {};
  return Torch{.available = device_.isTorchAvailable == YES, .enabled = device_.isTorchActive == YES};
}

- (void)publishLighting {
  if (lightingChanged_ && changed_ && session_.isRunning && !failed_) {
    lightingChanged_([self photoFlash], [self torch]);
  }
}

- (void)observeLighting:(std::function<void(PhotoFlash, std::optional<Torch>)>)changed {
  dispatch_async(queue_, ^{
    if (!changed_ || !session_.isRunning || failed_) return;
    if (!lightingChanged_) {
      if (device_.hasTorch) {
        [device_ addObserver:self forKeyPath:@"torchAvailable" options:0 context:&lighting_observation];
        [device_ addObserver:self forKeyPath:@"torchActive" options:0 context:&lighting_observation];
      }
      if (@available(macOS 13.0, *)) {
        [photoOutput_ addObserver:self forKeyPath:@"supportedFlashModes" options:0 context:&lighting_observation];
      }
    }
    lightingChanged_ = changed;
    [self publishLighting];
  });
}

- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object
                       change:(NSDictionary<NSKeyValueChangeKey, id>*)change context:(void*)context {
  if (context != &lighting_observation) {
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    return;
  }
  dispatch_async(queue_, ^{
    if (object == device_ || object == photoOutput_) [self publishLighting];
  });
}

- (CameraResult<void>)applyTorchEnabled:(bool)enabled {
  const auto torch = [self torch];
  if (!torch || (enabled && !torch->available)) {
    return CameraResult<void>::Failure({CameraErrorCode::Unavailable, "The camera torch is unavailable."});
  }
  NSError* error = nil;
  if (![device_ lockForConfiguration:&error]) {
    return CameraResult<void>::Failure({CameraErrorCode::CaptureFailed,
        error.localizedDescription.UTF8String ?: "Cannot configure the camera torch."});
  }
  @try {
    device_.torchMode = enabled ? AVCaptureTorchModeOn : AVCaptureTorchModeOff;
  } @catch (NSException* exception) {
    return CameraResult<void>::Failure({CameraErrorCode::CaptureFailed,
        exception.reason.UTF8String ?: "Cannot change the camera torch."});
  } @finally {
    [device_ unlockForConfiguration];
  }
  return CameraResult<void>::Success();
}

- (void)setTorchEnabled:(bool)enabled completed:(std::function<void(CameraResult<void>)>)completed {
  dispatch_async(queue_, ^{
    if (!changed_ || !session_.isRunning || failed_) {
      completed(CameraResult<void>::Failure({CameraErrorCode::NotReady, "The camera is not running."}));
      return;
    }
    auto result = [self applyTorchEnabled:enabled];
    [self publishLighting];
    completed(std::move(result));
  });
}

- (void)stopObservingLighting {
  if (!lightingChanged_) return;
  if (device_.hasTorch) {
    [device_ removeObserver:self forKeyPath:@"torchAvailable" context:&lighting_observation];
    [device_ removeObserver:self forKeyPath:@"torchActive" context:&lighting_observation];
  }
  if (@available(macOS 13.0, *)) {
    [photoOutput_ removeObserver:self forKeyPath:@"supportedFlashModes" context:&lighting_observation];
  }
  lightingChanged_ = {};
}

- (void)turnOffTorch {
  if (device_.torchMode != AVCaptureTorchModeOff) {
    CAMERA_UNUSED([self applyTorchEnabled:false]);
  }
}

- (void)capturePhoto:(PhotoOptions)options completed:(std::function<void(CameraResult<ImageAsset>)>)completed {
  dispatch_async(queue_, ^{
    if (!changed_ || !session_.isRunning || failed_) {
      completed(
          CameraResult<ImageAsset>::Failure({CameraErrorCode::NotReady, "Start the camera before taking a photo."}));
      return;
    }
    if (photo_) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::OperationInProgress, "A photo is still being processed."}));
      return;
    }
    if (!photoOutput_) {
      completed(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::Unavailable, "This camera cannot combine preview and still photos."}));
      return;
    }
    if (options.flash_mode != FlashMode::Off) {
      const auto flash = [self photoFlash];
      const bool supported = options.flash_mode == FlashMode::Auto ? flash.supports_auto : flash.supports_on;
      if (!supported || !device_.isFlashAvailable || device_.torchMode == AVCaptureTorchModeOn) {
        completed(CameraResult<ImageAsset>::Failure(
            {CameraErrorCode::Unavailable, "The requested flash mode is unavailable."}));
        return;
      }
    }
    AVCaptureConnection* connection = [photoOutput_ connectionWithMediaType:AVMediaTypeVideo];
    if (connection.isVideoMirroringSupported) {
      connection.automaticallyAdjustsVideoMirroring = NO;
      connection.videoMirrored = NO;
    }
    const auto token = photoGeneration_;
    auto finish = [self, token, completed](CameraResult<ImageAsset> result) {
      dispatch_async(queue_, ^{
        photo_ = nil;
        if (!changed_ || token != photoGeneration_) {
          completed(
              CameraResult<ImageAsset>::Failure({CameraErrorCode::Interrupted, "Photo capture was interrupted."}));
        } else {
          completed(result);
        }
        [self completeStops];
      });
    };
    if (!photoContext_) {
      photoContext_ = [CIContext contextWithOptions:@{kCIContextCacheIntermediates : @NO}];
    }
    photo_ = [[HUXMacPhotoCapture alloc] initWithOptions:options
                                                queue:photoQueue_
                                              context:photoContext_
                                            completed:finish];
    @try {
      AVCapturePhotoSettings* settings =
          [AVCapturePhotoSettings photoSettingsWithFormat:@{AVVideoCodecKey : AVVideoCodecTypeJPEG}];
      if (@available(macOS 13.0, *)) {
        if (options.flash_mode != FlashMode::Off) {
          settings.flashMode = options.flash_mode == FlashMode::Auto ? AVCaptureFlashModeAuto : AVCaptureFlashModeOn;
        }
      }
      [photoOutput_ capturePhotoWithSettings:settings delegate:photo_];
    } @catch (NSException* exception) {
      finish(CameraResult<ImageAsset>::Failure(
          {CameraErrorCode::CaptureFailed, exception.reason.UTF8String ?: "Cannot capture a photo."}));
    }
  });
}

- (void)stop:(std::function<void()>)completed {
  dispatch_async(queue_, ^{
    changed_ = {};
    ++photoGeneration_;
    if (completed)
      stops_.push_back(completed);
    for (id observer in observers_) {
      [NSNotificationCenter.defaultCenter removeObserver:observer];
    }
    [observers_ removeAllObjects];
    [self stopObservingLighting];
    [self turnOffTorch];
    [output_ setSampleBufferDelegate:nil queue:nullptr];
    [session_ stopRunning];
    producer_.processor = {};
    producer_.resizer.Reset();
    output_ = nil;
    device_ = nil;
    if (texture_) {
      texture_->Finish();
      texture_.reset();
    }
    preview_.reset();
    frames_.reset();
    [self completeStops];
  });
}

- (void)completeStops {
  if (changed_ || photo_)
    return;
  photoOutput_ = nil;
  session_ = nil;
  auto stops = std::exchange(stops_, {});
  for (const auto& completed : stops)
    completed();
}

@end

namespace huxerui::camera::detail {
namespace {

class AppleCamera final : public CameraBackend {
public:
  AppleCamera() : capture_([HUXCameraCapture new]) {}
  void Start(std::optional<Facing> facing, StatusCallback changed) override {
    [capture_ start:facing frames:frame_pipeline_ changed:std::move(changed)];
  }
  void Stop(std::function<void()> completed) override {
    [capture_ stop:std::move(completed)];
  }
  void ObserveLighting(std::function<void(PhotoFlash, std::optional<Torch>)> changed) override {
    [capture_ observeLighting:std::move(changed)];
  }
  void SetTorchEnabled(bool enabled, std::function<void(CameraResult<void>)> completed) override {
    [capture_ setTorchEnabled:enabled completed:std::move(completed)];
  }
  void FocusAndMeter(Point point, std::function<void(CameraResult<void>)> completed) override {
    [capture_ focusAndMeter:point completed:std::move(completed)];
  }

  void CapturePhoto(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) override {
    [capture_ capturePhoto:options completed:std::move(completed)];
  }

private:
  HUXCameraCapture* capture_;
};

} // namespace

void InstallPlatformCamera(ApplicationContext& context) {
  context.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, [](UiWindow&) -> std::shared_ptr<CameraBackend> {
        return std::make_shared<AppleCamera>();
      }
  );
}

} // namespace huxerui::camera::detail
