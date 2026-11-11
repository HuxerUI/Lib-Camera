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
using huxerui::camera::detail::FramePipeline;

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
  std::optional<PreviewOutput> preview_;
  std::optional<Facing> facing_;
  std::function<void(CameraStatus)> changed_;
}
- (void)start:(std::optional<Facing>)facing frames:(std::shared_ptr<FramePipeline>)frames
      changed:(std::function<void(CameraStatus)>)changed;
- (void)stop:(std::function<void()>)completed;
- (void)capturePhoto:(PhotoOptions)options completed:(std::function<void(CameraResult<ImageAsset>)>)completed;
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
  if (changed_) {
    changed_({.state = SessionState::Failed,
              .error = CameraError{code, message.UTF8String ?: "Camera capture failed."}});
  }
}

- (void)start:(std::optional<Facing>)facing frames:(std::shared_ptr<FramePipeline>)frames
      changed:(std::function<void(CameraStatus)>)changed {
  dispatch_async(queue_, ^{
    changed_ = changed;
    frames_ = frames;
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
    changed_({.state = SessionState::Running, .actual_facing = facing_});
  });
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection*)connection {
  if (output != output_ || !changed_) {
    return;
  }
  CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
  if (!buffer) {
    return;
  }
  try {
    const huxerui::Size size{static_cast<float>(CVPixelBufferGetWidth(buffer)),
                    static_cast<float>(CVPixelBufferGetHeight(buffer))};
    if (!texture_ || texture_->IntrinsicSize() != size) {
      if (texture_) {
        texture_->Finish();
      }
      texture_ = std::make_shared<macos::PixelBufferTexture>(size);
    }
    texture_->Publish(buffer);
    PreviewOutput frame{.texture = texture_, .mirrored = static_cast<bool>(connection.videoMirrored)};
    if (frames_) {
      const auto time = CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample), 1'000'000'000,
                                          kCMTimeRoundingMethod_Default);
      if (!CMTIME_IS_NUMERIC(time)) return;
      auto processed = frames_->Process(std::move(frame), std::chrono::nanoseconds(time.value));
      if (!processed.Succeeded()) {
        changed_({.state = SessionState::Failed, .error = processed.Error()});
        return;
      }
      frame = std::move(processed).Value();
    }
    if (frame != preview_) {
      preview_ = std::move(frame);
      changed_({.state = SessionState::Running, .actual_facing = facing_, .preview = preview_});
    }
  } catch (const std::exception& error) {
    [self fail:CameraErrorCode::CaptureFailed message:[NSString stringWithUTF8String:error.what()]];
  }
}

- (void)capturePhoto:(PhotoOptions)options completed:(std::function<void(CameraResult<ImageAsset>)>)completed {
  dispatch_async(queue_, ^{
    if (!changed_ || !session_.isRunning) {
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
    [output_ setSampleBufferDelegate:nil queue:nullptr];
    [session_ stopRunning];
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
