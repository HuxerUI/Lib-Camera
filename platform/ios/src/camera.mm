#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <vector>
#import <UIKit/UIKit.h>

#include <exception>
#include <utility>

#include <huxerui/ios/external_texture.h>
#include <huxerui/ios/platform_registry.h>

#include "camera_internal.h"

#define CAMERA_UNUSED(name) static_cast<void>(name)

using namespace huxerui;
using namespace huxerui::camera;
using huxerui::camera::detail::CameraBackend;
using huxerui::camera::detail::CameraDevicesResult;
using huxerui::camera::detail::CaptureFormatRank;
using huxerui::camera::detail::CapturePreferences;
using huxerui::camera::detail::FramePipeline;
using huxerui::camera::detail::FrameProducer;
using huxerui::camera::detail::ProcessedFrame;
using huxerui::camera::detail::full_texture_crop;

namespace {

char lighting_observation;

NSArray<AVCaptureDevice*>* DiscoverCameras() {
  NSMutableArray<AVCaptureDeviceType>* types = [@[
      AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeBuiltInUltraWideCamera,
      AVCaptureDeviceTypeBuiltInTelephotoCamera, AVCaptureDeviceTypeBuiltInTrueDepthCamera,
      AVCaptureDeviceTypeBuiltInDualCamera, AVCaptureDeviceTypeBuiltInDualWideCamera,
      AVCaptureDeviceTypeBuiltInTripleCamera] mutableCopy];
  if (@available(iOS 17.0, *)) {
    [types addObjectsFromArray:@[AVCaptureDeviceTypeExternal, AVCaptureDeviceTypeContinuityCamera]];
  }
  return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo
                                                                  position:AVCaptureDevicePositionUnspecified].devices;
}

std::optional<Facing> CameraFacing(AVCaptureDevice* device) {
  if (device.position == AVCaptureDevicePositionFront) return Facing::Front;
  if (device.position == AVCaptureDevicePositionBack) return Facing::Back;
  return {};
}

std::optional<std::string> CameraId(AVCaptureDevice* device) {
  if (const char* id = device.uniqueID.UTF8String; id && *id) return std::string(id);
  return {};
}

CameraDevicesResult EnumerateCameras() {
  @autoreleasepool {
    std::vector<CameraDevice> devices;
    for (AVCaptureDevice* device in DiscoverCameras()) {
      if (auto id = CameraId(device); id && device.isConnected) {
        devices.push_back({std::move(*id), device.localizedName.UTF8String ?: "Camera", CameraFacing(device)});
      }
    }
    return CameraDevicesResult::Success(std::move(devices));
  }
}

AVCaptureDevice* CameraWithId(const std::string& id) {
  for (AVCaptureDevice* device in DiscoverCameras()) {
    if (device.isConnected && CameraId(device) == id) return device;
  }
  return nil;
}

bool IsExternalCamera(AVCaptureDevice* device) {
  if (@available(iOS 17.0, *)) {
    return [device.deviceType isEqualToString:AVCaptureDeviceTypeExternal] ||
           [device.deviceType isEqualToString:AVCaptureDeviceTypeContinuityCamera];
  }
  return false;
}

std::optional<FrameRateRange> ReadFrameRate(AVCaptureDevice* device) {
  const double shortest = CMTimeGetSeconds(device.activeVideoMinFrameDuration);
  const double longest = CMTimeGetSeconds(device.activeVideoMaxFrameDuration);
  if (shortest <= 0 || longest < shortest || !std::isfinite(shortest) || !std::isfinite(longest)) return {};
  return FrameRateRange{1.0 / longest, 1.0 / shortest};
}

bool ConfigureCaptureFormat(AVCaptureDevice* device, const CapturePreferences& preferred, bool with_photo) {
  if (!preferred.pixel_width && !preferred.fps) return false;
  AVCaptureDeviceFormat* selected = nil;
  CMTime interval = kCMTimeInvalid;
  std::tuple<double, double, double> best;
  for (AVCaptureDeviceFormat* format in device.formats) {
    if (![format.mediaType isEqualToString:AVMediaTypeVideo]) continue;
    NSArray<Class>* unsupported = format.unsupportedCaptureOutputClasses;
    if ([unsupported containsObject:[AVCaptureVideoDataOutput class]] ||
        (with_photo && [unsupported containsObject:[AVCapturePhotoOutput class]])) continue;
    const auto size = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    if (size.width <= 0 || size.height <= 0) continue;
    CaptureFormat candidate{static_cast<std::uint32_t>(size.width), static_cast<std::uint32_t>(size.height), {}};
    if (!preferred.fps) {
      const auto rank = CaptureFormatRank(preferred, candidate);
      if (!selected || rank < best) {
        selected = format;
        best = rank;
      }
      continue;
    }
    for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
      if (range.minFrameRate <= 0 || range.maxFrameRate < range.minFrameRate ||
          !std::isfinite(range.minFrameRate) || !std::isfinite(range.maxFrameRate)) continue;
      candidate.frame_rate = FrameRateRange{range.minFrameRate, range.maxFrameRate};
      const auto rank = CaptureFormatRank(preferred, candidate);
      if (selected && rank >= best) continue;
      selected = format;
      best = rank;
      if (*preferred.fps <= range.minFrameRate) {
        interval = range.maxFrameDuration;
      } else if (*preferred.fps >= range.maxFrameRate) {
        interval = range.minFrameDuration;
      } else {
        interval = CMTimeMakeWithSeconds(1.0 / *preferred.fps, 1'000'000'000);
        if (CMTimeCompare(interval, range.minFrameDuration) < 0) interval = range.minFrameDuration;
        if (CMTimeCompare(interval, range.maxFrameDuration) > 0) interval = range.maxFrameDuration;
      }
    }
  }
  if (!selected || ![device lockForConfiguration:nil]) return false;
  @try {
    device.activeFormat = selected;
    if (CMTIME_IS_NUMERIC(interval) && interval.value > 0) {
      @try {
        device.activeVideoMinFrameDuration = interval;
        device.activeVideoMaxFrameDuration = interval;
      } @catch (NSException*) {
        // Keep the selected size and let the device choose the frame rate.
        device.activeVideoMinFrameDuration = kCMTimeInvalid;
        device.activeVideoMaxFrameDuration = kCMTimeInvalid;
      }
    }
    return true;
  } @catch (NSException*) {
    return false;
  } @finally {
    [device unlockForConfiguration];
  }
}

CMClockRef SessionClock(AVCaptureSession* session) {
  if (@available(iOS 15.4, *)) return session.synchronizationClock;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  return session.masterClock;
#pragma clang diagnostic pop
}

PreviewRotation RotationForOrientation(UIInterfaceOrientation orientation) {
  switch (orientation) {
    case UIInterfaceOrientationLandscapeLeft: return PreviewRotation::R90;
    case UIInterfaceOrientationPortraitUpsideDown: return PreviewRotation::R180;
    case UIInterfaceOrientationLandscapeRight: return PreviewRotation::R270;
    default: return PreviewRotation::R0;
  }
}

// AVFoundation normalizes sensor mounting into a fixed portrait buffer, including on landscape-mounted iPads.
// The orientation API remains available on the iOS 15 deployment target.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool ConfigurePreviewOutput(AVCaptureConnection* connection, bool external) {
  if (!connection) return false;
  if (external) {
    if (@available(iOS 17.0, *)) {
      if ([connection isVideoRotationAngleSupported:0.0]) connection.videoRotationAngle = 0.0;
    }
  } else {
    if (!connection.isVideoOrientationSupported) return false;
    connection.videoOrientation = AVCaptureVideoOrientationPortrait;
  }
  if (connection.isVideoMirroringSupported) {
    connection.automaticallyAdjustsVideoMirroring = NO;
    connection.videoMirrored = NO;
  }
  return true;
}
#pragma clang diagnostic pop

} // namespace

// A transparent child observes its owning window's transitions without replacing the application's delegates.
@interface HUXIOSCameraOrientation : UIViewController
@property(nonatomic, copy) void (^changed)(UIInterfaceOrientation);
- (void)updateOrientation;
- (UIInterfaceOrientation)orientation;
@end

@implementation HUXIOSCameraOrientation {
  UIInterfaceOrientation orientation_;
}

- (UIInterfaceOrientation)orientation {
  return orientation_ == UIInterfaceOrientationUnknown ? UIInterfaceOrientationPortrait : orientation_;
}

- (void)loadView {
  self.view = [UIView new];
  self.view.userInteractionEnabled = NO;
  self.view.accessibilityElementsHidden = YES;
  self.view.backgroundColor = UIColor.clearColor;
}

- (void)updateOrientation {
  UIWindowScene* scene = self.view.window.windowScene;
  if (!self.view.window) {
    return;
  }
  UIInterfaceOrientation orientation = UIInterfaceOrientationUnknown;
  if (scene) {
    if (@available(iOS 16.0, *)) {
      orientation = scene.effectiveGeometry.interfaceOrientation;
    } else {
      orientation = scene.interfaceOrientation;
    }
  } else {
    // Hosts using the UIApplication lifecycle do not have a UIWindowScene.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    orientation = self.parentViewController.interfaceOrientation;
#pragma clang diagnostic pop
  }
  if (orientation != UIInterfaceOrientationUnknown && orientation != orientation_) {
    orientation_ = orientation;
    if (self.changed) {
      self.changed(orientation);
    }
  }
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [self updateOrientation];
}

- (void)viewWillTransitionToSize:(CGSize)size
      withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
  [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
  __weak HUXIOSCameraOrientation* weak = self;
  [coordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext>) {
    [weak updateOrientation];
  }];
}

@end

namespace {

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

@interface HUXIOSPhotoCapture : NSObject <AVCapturePhotoCaptureDelegate> {
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

@implementation HUXIOSPhotoCapture

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

@interface HUXIOSCameraCapture : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
  __weak UIViewController* owner_;
  HUXIOSCameraOrientation* orientationObserver_;
  dispatch_queue_t queue_;
  AVCaptureSession* session_;
  AVCaptureVideoDataOutput* output_;
  AVCapturePhotoOutput* photoOutput_;
  HUXIOSPhotoCapture* photo_;
  dispatch_queue_t photoQueue_;
  CIContext* photoContext_;
  std::vector<std::function<void()>> stops_;
  std::uint64_t photoGeneration_;
  AVCaptureDevice* device_;
  NSMutableArray* observers_;
  std::shared_ptr<ios::PixelBufferTexture> texture_;
  std::shared_ptr<FramePipeline> frames_;
  FrameProducer producer_;
  std::optional<ProcessedFrame> preview_;
  std::optional<CaptureFormat> captureFormat_;
  std::optional<Facing> facing_;
  std::optional<std::string> deviceId_;
  bool externalCamera_;
  PreviewRotation rotation_;
  bool interrupted_;
  bool failed_;
  CameraBackend::StatusCallback changed_;
  std::function<void(PhotoFlash, std::optional<Torch>)> lightingChanged_;
}
- (instancetype)initWithOwner:(UIViewController*)owner;
- (void)start:(std::optional<Facing>)facing deviceId:(std::optional<std::string>)deviceId
    preferred:(CapturePreferences)preferred
       frames:(std::shared_ptr<FramePipeline>)frames
      changed:(CameraBackend::StatusCallback)changed;
- (void)stop:(std::function<void()>)completed;
- (void)observeLighting:(std::function<void(PhotoFlash, std::optional<Torch>)>)changed;
- (void)setTorchEnabled:(bool)enabled completed:(std::function<void(CameraResult<void>)>)completed;
- (void)turnOffTorch;
- (void)readZoom:(std::function<void(std::optional<Zoom>)>)completed;
- (void)setZoomRatio:(float)ratio completed:(std::function<void(CameraResult<Zoom>)>)completed;
- (void)readExposureCompensation:(std::function<void(std::optional<ExposureCompensation>)>)completed;
- (void)setExposureCompensation:(float)ev
                      completed:(std::function<void(CameraResult<ExposureCompensation>)>)completed;
- (void)capturePhoto:(PhotoOptions)options completed:(std::function<void(CameraResult<ImageAsset>)>)completed;
- (bool)supportsPointControl;
- (void)focusAndMeter:(huxerui::Point)point completed:(std::function<void(CameraResult<void>)>)completed;
@end

@implementation HUXIOSCameraCapture

- (instancetype)initWithOwner:(UIViewController*)owner {
  self = [super init];
  if (self) {
    owner_ = owner;
    queue_ = dispatch_queue_create("org.huxerui.camera.ios.capture", DISPATCH_QUEUE_SERIAL);
    observers_ = [NSMutableArray new];
    photoQueue_ = dispatch_queue_create("org.huxerui.camera.ios.photo", DISPATCH_QUEUE_SERIAL);
  }
  return self;
}

- (void)publish {
  if (!changed_ || failed_) {
    return;
  }
  CameraStatus status{.state = interrupted_ ? SessionState::Suspended : SessionState::Running,
                      .actual_facing = facing_,
                      .preview = preview_ ? std::optional{preview_->output} : std::nullopt,
                      .can_focus_and_meter = !interrupted_ && [self supportsPointControl],
                      .capture_format = captureFormat_, .actual_device_id = deviceId_};
  changed_(std::move(status), preview_ ? preview_->source_crop : full_texture_crop);
}

- (void)fail:(CameraErrorCode)code message:(NSString*)message {
  if (changed_ && !failed_) {
    failed_ = true;
    changed_({.state = SessionState::Failed,
              .error = CameraError{code, message.UTF8String ?: "Camera capture failed."}}, full_texture_crop);
  }
}

- (void)observe:(NSNotificationName)name object:(id)object handler:(void (^)(NSNotification*))handler {
  __weak HUXIOSCameraCapture* weak = self;
  id token = [NSNotificationCenter.defaultCenter addObserverForName:name object:object queue:nil
                                                       usingBlock:^(NSNotification* notification) {
    HUXIOSCameraCapture* capture = weak;
    if (capture) {
      dispatch_async(capture->queue_, ^{
        if (capture->changed_ && (notification.object == capture->session_ ||
                                  notification.object == capture->device_)) {
          handler(notification);
        }
      });
    }
  }];
  [observers_ addObject:token];
}

- (void)start:(std::optional<Facing>)facing deviceId:(std::optional<std::string>)deviceId
    preferred:(CapturePreferences)preferred
       frames:(std::shared_ptr<FramePipeline>)frames
      changed:(CameraBackend::StatusCallback)changed {
  UIViewController* owner = owner_;
  if (!owner) {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "The camera window is no longer available."}},
            full_texture_crop);
    return;
  }
  orientationObserver_ = [HUXIOSCameraOrientation new];
  [owner addChildViewController:orientationObserver_];
  orientationObserver_.view.frame = owner.view.bounds;
  orientationObserver_.view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [owner.view addSubview:orientationObserver_.view];
  [orientationObserver_ didMoveToParentViewController:owner];
  __weak HUXIOSCameraCapture* weak = self;
  orientationObserver_.changed = ^(UIInterfaceOrientation orientation) {
    HUXIOSCameraCapture* capture = weak;
    if (capture) {
      dispatch_async(capture->queue_, ^{
        capture->rotation_ = RotationForOrientation(orientation);
      });
    }
  };
  [orientationObserver_ updateOrientation];

  dispatch_async(queue_, ^{
    changed_ = changed;
    frames_ = frames;
    failed_ = false;
    interrupted_ = false;
    facing_.reset();
    const AVCaptureDevicePosition position = !facing ? AVCaptureDevicePositionUnspecified :
        (*facing == Facing::Front ? AVCaptureDevicePositionFront : AVCaptureDevicePositionBack);
    if (deviceId) {
      device_ = CameraWithId(*deviceId);
    } else if (!facing) {
      device_ = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    } else {
      device_ = [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
                                mediaType:AVMediaTypeVideo position:position].devices.firstObject;
    }
    if (!device_.isConnected) {
      [self fail:CameraErrorCode::DeviceNotFound message:@"No camera matches the requested selection."];
      return;
    }
    facing_ = CameraFacing(device_);
    deviceId_ = CameraId(device_);
    externalCamera_ = IsExternalCamera(device_);
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device_ error:&error];
    if (!input) {
      if (!device_.isConnected) {
        [self fail:CameraErrorCode::DeviceNotFound message:@"The selected camera is no longer connected."];
        return;
      }
      const auto code = error.code == AVErrorDeviceInUseByAnotherApplication ? CameraErrorCode::DeviceBusy :
          (error.code == AVErrorApplicationIsNotAuthorizedToUseDevice ? CameraErrorCode::PermissionDenied :
                                                                     CameraErrorCode::ConfigurationFailed);
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
    if (![output_.availableVideoCVPixelFormatTypes containsObject:@(kCVPixelFormatType_32BGRA)]) {
      [session_ commitConfiguration];
      [self fail:CameraErrorCode::ConfigurationFailed message:@"The camera cannot provide BGRA preview frames."];
      return;
    }
    output_.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
    if (!ConfigurePreviewOutput([output_ connectionWithMediaType:AVMediaTypeVideo], externalCamera_)) {
      [session_ commitConfiguration];
      [self fail:CameraErrorCode::ConfigurationFailed message:@"The camera cannot provide portrait preview frames."];
      return;
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
    if (preferred.pixel_width || preferred.fps) {
      if (ConfigureCaptureFormat(device_, preferred, photoOutput_ != nil)) {
        output_.automaticallyConfiguresOutputBufferDimensions = NO;
      } else if ([session_ canSetSessionPreset:AVCaptureSessionPreset1280x720]) {
        session_.sessionPreset = AVCaptureSessionPreset1280x720;
      }
    }
    [output_ setSampleBufferDelegate:self queue:queue_];
    [session_ commitConfiguration];

    [self observe:AVCaptureSessionRuntimeErrorNotification object:session_ handler:^(NSNotification* notification) {
      NSError* runtimeError = notification.userInfo[AVCaptureSessionErrorKey];
      HUXIOSCameraCapture* capture = weak;
      if (capture) {
        const auto code = runtimeError.code == AVErrorDeviceWasDisconnected || !capture->device_.isConnected
            ? CameraErrorCode::DeviceDisconnected : CameraErrorCode::CaptureFailed;
        [capture fail:code message:runtimeError.localizedDescription];
      }
    }];
    [self observe:AVCaptureDeviceWasDisconnectedNotification object:device_ handler:^(NSNotification*) {
      [weak fail:CameraErrorCode::DeviceDisconnected message:@"The camera was disconnected."];
    }];
    [self observe:AVCaptureSessionWasInterruptedNotification object:session_ handler:^(NSNotification*) {
      HUXIOSCameraCapture* capture = weak;
      if (capture) {
        capture->interrupted_ = true;
        ++capture->photoGeneration_;
        [capture turnOffTorch];
        [capture publish];
      }
    }];
    [self observe:AVCaptureSessionInterruptionEndedNotification object:session_ handler:^(NSNotification*) {
      HUXIOSCameraCapture* capture = weak;
      if (capture && !capture->failed_) {
        capture->interrupted_ = false;
        if (!capture->session_.isRunning) {
          [capture->session_ startRunning];
        }
        if (capture->session_.isRunning) {
          [capture turnOffTorch];
          [capture publish];
        } else {
          [capture fail:CameraErrorCode::CaptureFailed message:@"The camera did not resume after interruption."];
        }
      }
    }];
    [session_ startRunning];
    if (!session_.isRunning) {
      [self fail:CameraErrorCode::CaptureFailed message:@"The camera did not start."];
      return;
    }
    interrupted_ = session_.isInterrupted;
    [self publish];
  });
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection*)connection {
  CAMERA_UNUSED(connection);
  if (output != output_ || !changed_ || failed_ || interrupted_) {
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
      texture_ = std::make_shared<ios::PixelBufferTexture>(size);
    }
    texture_->Publish(buffer);
    ProcessedFrame frame{.output = {.texture = texture_,
                                   .rotation = externalCamera_ ? PreviewRotation::R0 : rotation_}};
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
    const CaptureFormat capture_format{width, height, ReadFrameRate(device_)};
    if (frame.output.texture && (frame != preview_ || capture_format != captureFormat_)) {
      preview_ = std::move(frame);
      captureFormat_ = capture_format;
      [self publish];
    }
  } catch (const std::exception& error) {
    [self fail:CameraErrorCode::CaptureFailed message:[NSString stringWithUTF8String:error.what()]];
  }
}

- (std::optional<Zoom>)zoomState {
  if (!changed_ || !session_.isRunning || interrupted_ || failed_) return {};
  const auto minimum = device_.minAvailableVideoZoomFactor;
  const auto maximum = device_.maxAvailableVideoZoomFactor;
  if (minimum >= maximum) return {};
  return Zoom{.ratio = static_cast<float>(device_.videoZoomFactor),
              .min_ratio = static_cast<float>(minimum),
              .max_ratio = static_cast<float>(maximum)};
}

- (void)readZoom:(std::function<void(std::optional<Zoom>)>)completed {
  dispatch_async(queue_, ^{
    completed([self zoomState]);
  });
}

- (void)setZoomRatio:(float)ratio completed:(std::function<void(CameraResult<Zoom>)>)completed {
  dispatch_async(queue_, ^{
    if (!changed_ || !session_.isRunning || interrupted_ || failed_) {
      completed(CameraResult<Zoom>::Failure({CameraErrorCode::NotReady, "The camera is not running."}));
      return;
    }
    NSError* error = nil;
    if (![device_ lockForConfiguration:&error]) {
      completed(CameraResult<Zoom>::Failure(
          {CameraErrorCode::CaptureFailed, error.localizedDescription.UTF8String ?: "Cannot configure camera zoom."}));
      return;
    }
    std::optional<Zoom> applied;
    NSString* failure = nil;
    @try {
      const auto minimum = device_.minAvailableVideoZoomFactor;
      const auto maximum = device_.maxAvailableVideoZoomFactor;
      if (minimum < maximum) {
        device_.videoZoomFactor = std::clamp(static_cast<CGFloat>(ratio), minimum, maximum);
        applied = [self zoomState];
      }
    } @catch (NSException* exception) {
      failure = exception.reason ?: @"Cannot apply camera zoom.";
    } @finally {
      [device_ unlockForConfiguration];
    }
    if (applied) {
      completed(CameraResult<Zoom>::Success(*applied));
    } else if (failure) {
      completed(CameraResult<Zoom>::Failure({CameraErrorCode::CaptureFailed, failure.UTF8String}));
    } else {
      completed(CameraResult<Zoom>::Failure({CameraErrorCode::Unavailable, "Camera zoom is unavailable."}));
    }
  });
}

- (std::optional<ExposureCompensation>)exposureCompensation {
  if (!device_ || !changed_ || !session_.isRunning || interrupted_ || failed_) return {};
  const auto minimum = device_.minExposureTargetBias;
  const auto maximum = device_.maxExposureTargetBias;
  if (minimum >= maximum) return {};
  return ExposureCompensation{.ev = device_.exposureTargetBias, .min_ev = minimum, .max_ev = maximum};
}

- (void)readExposureCompensation:(std::function<void(std::optional<ExposureCompensation>)>)completed {
  dispatch_async(queue_, ^{
    completed([self exposureCompensation]);
  });
}

- (void)setExposureCompensation:(float)ev
                      completed:(std::function<void(CameraResult<ExposureCompensation>)>)completed {
  dispatch_async(queue_, ^{
    if (!device_ || !changed_ || !session_.isRunning || interrupted_ || failed_) {
      completed(CameraResult<ExposureCompensation>::Failure(
          {CameraErrorCode::NotReady, "The camera is not running."}));
      return;
    }
    NSError* error = nil;
    if (![device_ lockForConfiguration:&error]) {
      completed(CameraResult<ExposureCompensation>::Failure({CameraErrorCode::CaptureFailed,
          error.localizedDescription.UTF8String ?: "Cannot configure camera exposure compensation."}));
      return;
    }
    @try {
      const auto exposure = [self exposureCompensation];
      if (!exposure) {
        completed(CameraResult<ExposureCompensation>::Failure(
            {CameraErrorCode::Unavailable, "Camera exposure compensation is unavailable."}));
        return;
      }
      const auto target = std::clamp(ev, exposure->min_ev, exposure->max_ev);
      const auto queue = queue_;
      __weak HUXIOSCameraCapture* weakSelf = self;
      __weak AVCaptureDevice* weakDevice = device_;
      [device_ setExposureTargetBias:target completionHandler:^(CMTime) {
        dispatch_async(queue, ^{
          HUXIOSCameraCapture* capture = weakSelf;
          AVCaptureDevice* device = weakDevice;
          if (capture && device && capture->device_ == device) {
            if (const auto applied = [capture exposureCompensation]) {
              completed(CameraResult<ExposureCompensation>::Success(*applied));
              return;
            }
          }
          completed(CameraResult<ExposureCompensation>::Failure(
              {CameraErrorCode::Interrupted, "Camera exposure compensation was interrupted."}));
        });
      }];
    } @catch (NSException* exception) {
      completed(CameraResult<ExposureCompensation>::Failure({CameraErrorCode::CaptureFailed,
          exception.reason.UTF8String ?: "Cannot apply camera exposure compensation."}));
    } @finally {
      [device_ unlockForConfiguration];
    }
  });
}

- (bool)supportsPointControl {
  return (device_.isFocusPointOfInterestSupported &&
          [device_ isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus]) ||
         (device_.isExposurePointOfInterestSupported &&
          [device_ isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]);
}

- (void)focusAndMeter:(huxerui::Point)point completed:(std::function<void(CameraResult<void>)>)completed {
  dispatch_async(queue_, ^{
    if (!device_ || !changed_ || !session_.isRunning || interrupted_ || failed_ || !preview_) {
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
      // Built-in cameras deliver portrait buffers. Device points use the source orientation.
      const CGPoint target = externalCamera_ ? CGPointMake(point.x, point.y) : CGPointMake(point.y, 1.0F - point.x);
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
  if (lightingChanged_ && changed_ && session_.isRunning && !failed_ && !interrupted_) {
    lightingChanged_([self photoFlash], [self torch]);
  }
}

- (void)observeLighting:(std::function<void(PhotoFlash, std::optional<Torch>)>)changed {
  dispatch_async(queue_, ^{
    if (!changed_ || !session_.isRunning || failed_ || interrupted_) return;
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
    if (!changed_ || !session_.isRunning || failed_ || interrupted_) {
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
  const auto orientation = [orientationObserver_ orientation];
  dispatch_async(queue_, ^{
    if (!changed_ || !session_.isRunning || interrupted_ || failed_) {
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
    if (externalCamera_) {
      if (@available(iOS 17.0, *)) {
        if ([connection isVideoRotationAngleSupported:0.0]) connection.videoRotationAngle = 0.0;
      }
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (!externalCamera_ && connection.isVideoOrientationSupported) {
      switch (orientation) {
      case UIInterfaceOrientationLandscapeLeft:
        connection.videoOrientation = AVCaptureVideoOrientationLandscapeLeft;
        break;
      case UIInterfaceOrientationLandscapeRight:
        connection.videoOrientation = AVCaptureVideoOrientationLandscapeRight;
        break;
      case UIInterfaceOrientationPortraitUpsideDown:
        connection.videoOrientation = AVCaptureVideoOrientationPortraitUpsideDown;
        break;
      default:
        connection.videoOrientation = AVCaptureVideoOrientationPortrait;
        break;
      }
    }
#pragma clang diagnostic pop
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
    photo_ = [[HUXIOSPhotoCapture alloc] initWithOptions:options
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
  orientationObserver_.changed = nil;
  [orientationObserver_ willMoveToParentViewController:nil];
  [orientationObserver_.view removeFromSuperview];
  [orientationObserver_ removeFromParentViewController];
  orientationObserver_ = nil;
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
    captureFormat_.reset();
    deviceId_.reset();
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

class IOSCamera final : public CameraBackend {
public:
  explicit IOSCamera(UIViewController* owner) : capture_([[HUXIOSCameraCapture alloc] initWithOwner:owner]) {}

  Task<CameraDevicesResult> EnumerateDevices() override {
    co_return co_await RunWorker(&EnumerateCameras);
  }

  void Start(std::optional<Facing> facing, std::optional<std::string> device_id,
             CapturePreferences preferred, StatusCallback changed) override {
    [capture_ start:facing deviceId:std::move(device_id) preferred:preferred
              frames:frame_pipeline_ changed:std::move(changed)];
  }

  void Stop(std::function<void()> completed) override {
    [capture_ stop:std::move(completed)];
  }
  void ReadZoom(std::function<void(std::optional<Zoom>)> completed) override {
    [capture_ readZoom:std::move(completed)];
  }
  void SetZoomRatio(float ratio, std::function<void(CameraResult<Zoom>)> completed) override {
    [capture_ setZoomRatio:ratio completed:std::move(completed)];
  }
  void ReadExposureCompensation(std::function<void(std::optional<ExposureCompensation>)> completed) override {
    [capture_ readExposureCompensation:std::move(completed)];
  }
  void SetExposureCompensation(float ev, std::function<void(CameraResult<ExposureCompensation>)> completed) override {
    [capture_ setExposureCompensation:ev completed:std::move(completed)];
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
  HUXIOSCameraCapture* capture_;
};

} // namespace

void InstallPlatformCamera(ApplicationContext& context) {
  context.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, ios::WindowPlatformModuleFactory<std::shared_ptr<CameraBackend>>{
          .create = [](UiWindow&, UIViewController* owner) -> std::shared_ptr<CameraBackend> {
            return std::make_shared<IOSCamera>(owner);
          },
      }
  );
}

} // namespace huxerui::camera::detail
