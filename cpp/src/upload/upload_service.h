#pragma once
#include "upload/s3_client.h"
#include "upload/s3_target.h"
#include <QImage>
#include <QObject>
#include <functional>
#include <optional>

namespace jietuba {
class ToolSettingsManager;
}

namespace jietuba::upload {
enum class Source { Screenshot, Pin };

// What an upload ended in; link is the text that was copied.
struct Report {
    PutOutcome outcome;
    QString link;
    QString message;
};

// The target the upload buttons use: the default one, when it is still in the list.
std::optional<Target> default_target(ToolSettingsManager &config);

// Uploads the image to the default target in the background. A toast shows the progress and, when it ends,
// the result; on success the link goes to the clipboard in the chosen form. Returns false, doing nothing,
// when there is no default target. The ending is reported on the main thread to done when given; without
// done a failure also opens a warning with its hint, since a toast cannot hold it.
bool start_upload(const QImage &image, ToolSettingsManager &config, Source source, std::function<void(const Report &)> done = {});

// Tells the user to add a target first: used by every entry before it closes anything.
void show_no_target_hint();

// A 1x1 PNG put under the target's prefix as jietuba-test-<random>.png; done runs on the main thread while
// context lives, with the link of the object (the address that would be copied, without the link format).
void upload_test_image(const Target &target, QObject *context, std::function<void(const Report &)> done);
}
