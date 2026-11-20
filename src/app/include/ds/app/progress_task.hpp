// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <QProgressDialog>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <functional>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

/// @file
/// @brief Runs a long task (hashing elevation files, FR-TER-L06-like) on a background thread
/// behind a modal progress dialog with *Cancel*.
/// @ingroup app

namespace ds::app {

/// @brief Reports progress, 0 … 1 (callable from any thread).
using ProgressReport = std::function<void(double fraction)>;

/// @brief Runs @p work on a background thread and keeps the GUI responsive while it runs.
/// @details A modal progress dialog titled @p label appears after 400 ms. *Cancel* requests a
/// stop through the stop token; @p work should return soon after. The calling thread runs a
/// local event loop until @p work returns.
/// @return The result of @p work, or nullopt if the user cancelled.
template <typename T>
[[nodiscard]] std::optional<T> runWithProgress(
    QWidget* parent, const QString& label,
    std::function<T(std::stop_token, const ProgressReport&)> work) {
    std::atomic<int> permille{0};
    std::atomic<bool> finished{false};
    std::optional<T> result;
    const ProgressReport report = [&permille](double fraction) {
        permille.store(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 1000.0));
    };
    std::jthread worker([&](std::stop_token stop) {
        result.emplace(work(stop, report));
        finished.store(true);
    });

    QProgressDialog dialog(label, QCoreApplication::translate("ds::app", "Cancel"), 0, 1000,
                           parent);
    dialog.setObjectName(QStringLiteral("TaskProgressDialog"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(400);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    bool cancelled = false;
    QEventLoop loop;
    QTimer timer;
    QObject::connect(&dialog, &QProgressDialog::canceled, &dialog, [&] {
        cancelled = true;
        worker.request_stop();
    });
    QObject::connect(&timer, &QTimer::timeout, &dialog, [&] {
        if (finished.load()) {
            loop.quit();
            return;
        }
        dialog.setValue(permille.load());
    });
    timer.start(30);
    if (!finished.load()) {
        loop.exec();
    }
    worker.join();
    if (cancelled) {
        return std::nullopt;
    }
    return result;
}

}  // namespace ds::app
