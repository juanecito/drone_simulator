// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <QApplication>
#include <QMessageBox>
#include <QSurfaceFormat>

#include <print>
#include <string>
#include <vector>

#include "ds/app/command_line.hpp"
#include "ds/app/main_window.hpp"
#include "ds/app/opengl_check.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/common/log.hpp"
#include "ds/common/version.hpp"
#include "ds/graphics/renderer.hpp"
#include "ds/storage/data_location.hpp"

namespace {

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

// Requests the OpenGL 4.5 core context (FR-UI-01, FR-GFX-01). Must run before QApplication.
void setDefaultSurfaceFormat(bool debugContext) {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(ds::graphics::Renderer::kRequiredMajor,
                      ds::graphics::Renderer::kRequiredMinor);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    // No multisampling here: the renderer draws into its own multisampled HDR target and
    // only writes the tone-mapped result to the window (FR-GFX-15).
    format.setSamples(0);
    if (debugContext) {
        format.setOption(QSurfaceFormat::DebugContext);
    }
    QSurfaceFormat::setDefaultFormat(format);
}

void showFatalError(QWidget* parent, const QString& message) {
    QMessageBox::critical(parent, QObject::tr("Drone Simulator — Error"), message);
}

int usageError(const ds::common::Error& error) {
    std::println(stderr, "drone_simulator: {}\nTry 'drone_simulator --help'.", error.message);
    return kExitUsage;
}

}  // namespace

int main(int argc, char* argv[]) {
    // First pass before QApplication: --help/--version work without a display, and the
    // surface format has to be set before the application object exists.
    const std::vector<std::string> rawArgs(argv, argv + argc);
    const auto early = ds::app::parseCommandLine(rawArgs, ds::app::ParseMode::Lenient);
    if (!early) {
        return usageError(early.error());
    }
    if (early->showHelp) {
        std::print("{}", ds::app::helpText());
        return 0;
    }
    if (early->showVersion) {
        std::println("{} {}", ds::common::kApplicationName, ds::common::kVersion);
        return 0;
    }
    if (early->logLevel) {
        ds::log::setLevel(*early->logLevel);
    }
    setDefaultSurfaceFormat(early->glDebug || kDebugBuild);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("drone_simulator"));
    QApplication::setApplicationDisplayName(QStringLiteral("Drone Simulator"));
    QApplication::setApplicationVersion(QString::fromUtf8(
        ds::common::kVersion.data(), static_cast<qsizetype>(ds::common::kVersion.size())));
    QApplication::setDesktopFileName(QStringLiteral("drone-simulator"));

    // Second pass: Qt has removed its own options, so anything left over is an error.
    std::vector<std::string> args;
    for (const QString& arg : QApplication::arguments()) {
        args.push_back(arg.toStdString());
    }
    const auto options = ds::app::parseCommandLine(args, ds::app::ParseMode::Strict);
    if (!options) {
        return usageError(options.error());
    }

    ds::log::info("{} {} starting", ds::common::kApplicationName, ds::common::kVersion);

    // FR-GFX-01: clear error dialog and clean exit if OpenGL 4.5 core is not available.
    if (const auto gl = ds::app::checkOpenGlContext(); !gl) {
        ds::log::error("{}", gl.error().message);
        showFatalError(nullptr, QString::fromStdString(gl.error().message));
        return kExitFailure;
    }

    // FR-DB-01/02: the per-user database, or an in-memory one with a warning.
    auto storage = ds::app::openUserStorage(options->databaseFile, ds::storage::homeDirectory());
    if (!storage) {
        ds::log::error("{}", storage.error().message);
        showFatalError(nullptr, QString::fromStdString(storage.error().message));
        return kExitFailure;
    }
    if (!storage->warning.empty()) {
        QMessageBox::warning(nullptr, QObject::tr("Drone Simulator — Data Folder"),
                             QString::fromStdString(storage->warning));
    }
    ds::app::SettingsController settings(*storage->service);

    ds::app::MainWindow window(&settings);
    QObject::connect(&window, &ds::app::MainWindow::fatalError, &app,
                     [&window](const QString& message) {
                         showFatalError(&window, message);
                         QApplication::exit(kExitFailure);
                     });
    window.show();
    return QApplication::exec();
}
