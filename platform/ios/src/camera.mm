#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>

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
using huxerui::camera::detail::FramePipeline;

namespace {

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
bool ConfigurePortraitOutput(AVCaptureConnection* connection) {
  if (!connection.isVideoOrientationSupported) {
    return false;
  }
  connection.videoOrientation = AVCaptureVideoOrientationPortrait;
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
  FrameProcessor processor_;
  std::optional<PreviewOutput> preview_;
  std::optional<Facing> facing_;
  PreviewRotation rotation_;
  bool interrupted_;
  bool failed_;
  std::function<void(CameraStatus)> changed_;
}
- (instancetype)initWithOwner:(UIViewController*)owner;
- (void)start:(std::optional<Facing>)facing frames:(std::shared_ptr<FramePipeline>)frames
      changed:(std::function<void(CameraStatus)>)changed;
- (void)stop:(std::function<void()>)completed;
- (void)capturePhoto:(PhotoOptions)options completed:(std::function<void(CameraResult<ImageAsset>)>)completed;
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
                      .actual_facing = facing_, .preview = preview_};
  changed_(std::move(status));
}

- (void)fail:(CameraErrorCode)code message:(NSString*)message {
  if (changed_ && !failed_) {
    failed_ = true;
    changed_({.state = SessionState::Failed,
              .error = CameraError{code, message.UTF8String ?: "Camera capture failed."}});
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

- (void)start:(std::optional<Facing>)facing frames:(std::shared_ptr<FramePipeline>)frames
      changed:(std::function<void(CameraStatus)>)changed {
  UIViewController* owner = owner_;
  if (!owner) {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "The camera window is no longer available."}});
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
    if (!facing) {
      device_ = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    } else {
      device_ = [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera]
                                mediaType:AVMediaTypeVideo position:position].devices.firstObject;
    }
    if (!device_) {
      [self fail:CameraErrorCode::DeviceNotFound message:@"No camera matches the requested facing."];
      return;
    }
    if (device_.position == AVCaptureDevicePositionFront) {
      facing_ = Facing::Front;
    } else if (device_.position == AVCaptureDevicePositionBack) {
      facing_ = Facing::Back;
    }
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device_ error:&error];
    if (!input) {
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
    if (!ConfigurePortraitOutput([output_ connectionWithMediaType:AVMediaTypeVideo])) {
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
    [output_ setSampleBufferDelegate:self queue:queue_];
    [session_ commitConfiguration];

    [self observe:AVCaptureSessionRuntimeErrorNotification object:session_ handler:^(NSNotification* notification) {
      NSError* runtimeError = notification.userInfo[AVCaptureSessionErrorKey];
      [weak fail:CameraErrorCode::CaptureFailed message:runtimeError.localizedDescription];
    }];
    [self observe:AVCaptureDeviceWasDisconnectedNotification object:device_ handler:^(NSNotification*) {
      [weak fail:CameraErrorCode::DeviceDisconnected message:@"The camera was disconnected."];
    }];
    [self observe:AVCaptureSessionWasInterruptedNotification object:session_ handler:^(NSNotification*) {
      HUXIOSCameraCapture* capture = weak;
      if (capture) {
        capture->interrupted_ = true;
        ++capture->photoGeneration_;
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
      processor_ = {};
      if (texture_) {
        texture_->Finish();
      }
      texture_ = std::make_shared<ios::PixelBufferTexture>(size);
    }
    texture_->Publish(buffer);
    PreviewOutput frame{.texture = texture_, .rotation = rotation_};
    if (frames_) {
      const auto time = CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample), 1'000'000'000,
                                          kCMTimeRoundingMethod_Default);
      if (!CMTIME_IS_NUMERIC(time)) return;
      auto processed = frames_->Process(processor_, std::move(frame), std::chrono::nanoseconds(time.value), width, height);
      if (!processed.Succeeded()) {
        failed_ = true;
        changed_({.state = SessionState::Failed, .error = processed.Error()});
        return;
      }
      frame = std::move(processed).Value();
    }
    if (frame != preview_) {
      preview_ = std::move(frame);
      [self publish];
    }
  } catch (const std::exception& error) {
    [self fail:CameraErrorCode::CaptureFailed message:[NSString stringWithUTF8String:error.what()]];
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
    AVCaptureConnection* connection = [photoOutput_ connectionWithMediaType:AVMediaTypeVideo];
    if (connection.isVideoMirroringSupported) {
      connection.automaticallyAdjustsVideoMirroring = NO;
      connection.videoMirrored = NO;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (connection.isVideoOrientationSupported) {
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
    [output_ setSampleBufferDelegate:nil queue:nullptr];
    [session_ stopRunning];
    processor_ = {};
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

class IOSCamera final : public CameraBackend {
public:
  explicit IOSCamera(UIViewController* owner) : capture_([[HUXIOSCameraCapture alloc] initWithOwner:owner]) {}

  void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) override {
    [capture_ start:facing frames:frame_pipeline_ changed:std::move(changed)];
  }

  void Stop(std::function<void()> completed) override {
    [capture_ stop:std::move(completed)];
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
