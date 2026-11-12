#include "camera_test_fixture.h"

namespace huxerui::camera::test {
namespace {

[[huxerui::composable]]
View PhotoTestOwner(PhotoTestContext* context) {
  auto status = UseState(CameraStatus{.state = SessionState::Running, .actual_facing = context->facing});
  auto tasks = UseTaskScope();
  auto retained = UseState(std::make_shared<camera::detail::CameraSessionData>(status, tasks, UseApplication()));
  context->owner = retained;
  Lifecycle([owner = context->owner, backend = context->backend] {
    owner->backend = backend;
    owner->engaged = true;
    return [owner] { owner->Close(); };
  }, context->owner);
  return context->content;
}

PhotoTestContext* active_photo_test = nullptr;

[[huxerui::composable]]
View PhotoTestContent(PhotoTestContext* context) {
  context->callers = UseTaskScope();
  context->visible = UseState(true);
  return context->visible ? PhotoTestOwner(context) : View{};
}

View PhotoTestRoot() { return PhotoTestContent(active_photo_test); }

} // namespace

PhotoTestFixture::PhotoTestFixture(Facing facing, std::function<void(ApplicationContext&)> install, View content)
    : application(PhotoTestRoot, {.show_debug_overlay = false, .application_hooks = {
        [install = std::move(install)](ApplicationContext& app) { if (install) install(app); }}}) {
  Check(!active_photo_test, "Photo fixtures must not overlap.");
  active_photo_test = &context;
  context.facing = facing;
  context.content = std::move(content);
  platform.Start(application);
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  Drain();
}

PhotoTestFixture::~PhotoTestFixture() { active_photo_test = nullptr; }

} // namespace huxerui::camera::test
