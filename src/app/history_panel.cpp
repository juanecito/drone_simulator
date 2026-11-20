// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/history_panel.hpp"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <cmath>
#include <memory>

#include "ds/app/simulation_controller.hpp"
#include "ds/app/telemetry_plot.hpp"
#include "ds/storage/storage_service.hpp"

namespace ds::app {
namespace {

constexpr int kSessionLimit = 200;
constexpr int kAttemptLimit = 200;
constexpr int kPollMs = 20;

QString duration(double seconds) {
    const auto s = static_cast<long long>(std::llround(std::max(0.0, seconds)));
    return s >= 3600 ? QStringLiteral("%1:%2:%3")
                           .arg(s / 3600)
                           .arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
                           .arg(s % 60, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString shortTime(const std::string& iso) {
    // "2026-06-21T10:00:00Z" → "2026-06-21 10:00".
    QString t = QString::fromStdString(iso);
    t.replace(QLatin1Char('T'), QLatin1Char(' '));
    return t.left(16);
}

QTableWidgetItem* cell(const QString& text, bool number = false) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    if (number) {
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    return item;
}

QTableWidget* table(const QStringList& headers, const char* name) {
    auto* t = new QTableWidget(0, static_cast<int>(headers.size()));
    t->setObjectName(QString::fromLatin1(name));
    t->setHorizontalHeaderLabels(headers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->verticalHeader()->setVisible(false);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setAlternatingRowColors(true);
    return t;
}

}  // namespace

HistoryPanel::HistoryPanel(storage::StorageService* storage, SimulationController& simulation,
                           QWidget* parent)
    : QWidget(parent), storage_(storage), simulation_(&simulation) {
    buildUi();
    poll_ = new QTimer(this);
    poll_->setInterval(kPollMs);
    connect(poll_, &QTimer::timeout, this, &HistoryPanel::poll);
    connect(simulation_, &SimulationController::sessionRecorded, this, [this] { refresh(); });
    connect(simulation_, &SimulationController::replayActiveChanged, this,
            &HistoryPanel::updateButtons);
    connect(simulation_, &SimulationController::missionActiveChanged, this,
            &HistoryPanel::updateButtons);
    refresh();
}

void HistoryPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);
    if (storage_ == nullptr) {
        auto* label = new QLabel(tr("The history is not saved without a database."));
        label->setEnabled(false);
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);
        return;
    }
    auto* splitter = new QSplitter(Qt::Vertical);

    auto* sessionsBox = new QGroupBox(tr("Flight sessions"));
    auto* sessionsLayout = new QVBoxLayout(sessionsBox);
    sessionsTable_ = table(
        {tr("Started (sim UTC)"), tr("Vehicle"), tr("Mission"), tr("Duration"), tr("Max AGL (m)"),
         tr("Max speed (m/s)"), tr("Distance (m)"), tr("Energy (Wh)"), tr("Crashes"), tr("Replay")},
        "SessionsTable");
    sessionsLayout->addWidget(sessionsTable_);
    auto* buttons = new QHBoxLayout;
    refresh_ = new QPushButton(tr("Refresh"));
    refresh_->setObjectName(QStringLiteral("RefreshHistoryButton"));
    replay_ = new QPushButton(tr("▶ Replay"));
    replay_->setObjectName(QStringLiteral("ReplayButton"));
    replay_->setToolTip(tr("Plays the session again from its recorded controls (FR-X-06)"));
    stopReplay_ = new QPushButton(tr("■ Stop replay"));
    stopReplay_->setObjectName(QStringLiteral("StopReplayButton"));
    delete_ = new QPushButton(tr("Delete"));
    delete_->setObjectName(QStringLiteral("DeleteSessionButton"));
    buttons->addWidget(refresh_);
    buttons->addStretch();
    buttons->addWidget(replay_);
    buttons->addWidget(stopReplay_);
    buttons->addWidget(delete_);
    sessionsLayout->addLayout(buttons);
    splitter->addWidget(sessionsBox);

    auto* plotBox = new QGroupBox(tr("Telemetry"));
    auto* plotLayout = new QVBoxLayout(plotBox);
    auto* plotHeader = new QHBoxLayout;
    quantity_ = new QComboBox;
    quantity_->setObjectName(QStringLiteral("PlotQuantityCombo"));
    for (const PlotQuantity q : kPlotQuantities) {
        const std::string_view title = plotQuantityTitle(q);
        quantity_->addItem(
            tr(QByteArray(title.data(), static_cast<qsizetype>(title.size())).constData()),
            static_cast<int>(q));
    }
    summary_ = new QLabel;
    summary_->setObjectName(QStringLiteral("SessionSummaryLabel"));
    plotHeader->addWidget(quantity_);
    plotHeader->addWidget(summary_, 1);
    plotLayout->addLayout(plotHeader);
    plot_ = new TelemetryPlot;
    plot_->setObjectName(QStringLiteral("TelemetryPlot"));
    plotLayout->addWidget(plot_, 1);
    splitter->addWidget(plotBox);

    auto* attemptsBox = new QGroupBox(tr("Mission attempts"));
    auto* attemptsLayout = new QVBoxLayout(attemptsBox);
    attemptsTable_ = table({tr("Mission"), tr("Started (sim UTC)"), tr("Outcome"), tr("Stars"),
                            tr("Score"), tr("Time"), tr("Session")},
                           "AttemptsTable");
    attemptsLayout->addWidget(attemptsTable_);
    splitter->addWidget(attemptsBox);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 1);
    layout->addWidget(splitter, 1);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("HistoryStatusLabel"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    connect(refresh_, &QPushButton::clicked, this, &HistoryPanel::refresh);
    connect(replay_, &QPushButton::clicked, this, &HistoryPanel::requestReplay);
    connect(stopReplay_, &QPushButton::clicked, simulation_, &SimulationController::stopReplay);
    connect(delete_, &QPushButton::clicked, this, &HistoryPanel::confirmRemove);
    connect(quantity_, &QComboBox::currentIndexChanged, this, [this](int index) {
        plot_->setQuantity(static_cast<PlotQuantity>(quantity_->itemData(index).toInt()));
    });
    connect(sessionsTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        updateButtons();
        if (const auto id = selectedSession()) {
            loadTelemetry(*id);
        } else {
            plot_->setSamples({});
            summary_->clear();
        }
    });
    connect(attemptsTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = attemptsTable_->currentRow();
        if (row < 0 || row >= static_cast<int>(attempts_.size())) {
            return;
        }
        if (const auto session = attempts_[static_cast<std::size_t>(row)].sessionId) {
            selectSession(*session);
        }
    });
    updateButtons();
}

template <typename T>
void HistoryPanel::whenReady(std::future<T> future, std::function<void(T)> done) {
    auto shared = std::make_shared<std::future<T>>(std::move(future));
    pending_.push_back([shared, done = std::move(done)] {
        if (shared->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return false;
        }
        done(shared->get());
        return true;
    });
    poll_->start();
}

void HistoryPanel::poll() {
    // Callbacks may queue more requests: run the ready ones from a copy.
    auto pending = std::exchange(pending_, {});
    std::vector<std::function<bool()>> waiting;
    for (auto& p : pending) {
        if (!p()) {
            waiting.push_back(std::move(p));
        }
    }
    for (auto& p : pending_) {
        waiting.push_back(std::move(p));
    }
    pending_ = std::move(waiting);
    if (pending_.empty()) {
        poll_->stop();
    }
}

void HistoryPanel::refresh() {
    if (storage_ == nullptr) {
        return;
    }
    whenReady<storage::StorageResult<std::vector<storage::SessionRecord>>>(
        storage_->submit([](storage::DbConnection& c) {
            return storage::SessionStore(c).recent(kSessionLimit);
        }),
        [this](storage::StorageResult<std::vector<storage::SessionRecord>> sessions) {
            if (!sessions) {
                status_->setText(tr("Cannot read the sessions: %1")
                                     .arg(QString::fromStdString(sessions.error().message)));
                return;
            }
            sessions_ = std::move(*sessions);
            fillSessions();
        });
    whenReady<storage::StorageResult<std::vector<storage::MissionAttemptRecord>>>(
        storage_->submit([](storage::DbConnection& c) {
            return storage::MissionStore(c).attempts({}, kAttemptLimit);
        }),
        [this](storage::StorageResult<std::vector<storage::MissionAttemptRecord>> attempts) {
            if (!attempts) {
                status_->setText(tr("Cannot read the mission attempts: %1")
                                     .arg(QString::fromStdString(attempts.error().message)));
                return;
            }
            attempts_ = std::move(*attempts);
            fillAttempts();
        });
}

void HistoryPanel::fillSessions() {
    const std::optional<std::int64_t> selected = selectedSession();
    const QSignalBlocker block(sessionsTable_);
    sessionsTable_->setRowCount(static_cast<int>(sessions_.size()));
    int reselect = -1;
    for (int row = 0; row < static_cast<int>(sessions_.size()); ++row) {
        const storage::SessionRecord& s = sessions_[static_cast<std::size_t>(row)];
        const QString vehicle = s.vehicleProfileKey.empty()
                                    ? QString::fromStdString(s.vehicleType)
                                    : QString::fromStdString(s.vehicleProfileKey);
        sessionsTable_->setItem(row, 0, cell(shortTime(s.startedAt)));
        sessionsTable_->setItem(row, 1, cell(vehicle));
        sessionsTable_->setItem(
            row, 2,
            cell(s.missionKey.empty() ? tr("free flight") : QString::fromStdString(s.missionKey)));
        sessionsTable_->setItem(row, 3, cell(duration(s.durationS), true));
        sessionsTable_->setItem(row, 4, cell(QString::number(s.maxAltitudeAglM, 'f', 1), true));
        sessionsTable_->setItem(row, 5, cell(QString::number(s.maxSpeedMs, 'f', 1), true));
        sessionsTable_->setItem(row, 6, cell(QString::number(s.distanceM, 'f', 0), true));
        sessionsTable_->setItem(row, 7, cell(QString::number(s.energyUsedWh, 'f', 2), true));
        sessionsTable_->setItem(row, 8, cell(QString::number(s.crashes), true));
        sessionsTable_->setItem(row, 9, cell(s.hasReplay ? tr("yes") : tr("—")));
        if (selected && s.id == selected) {
            reselect = row;
        }
    }
    if (reselect >= 0) {
        sessionsTable_->selectRow(reselect);
    } else {
        sessionsTable_->clearSelection();
        plot_->setSamples({});
        summary_->clear();
    }
    status_->setText(sessions_.empty()
                         ? tr("No flight recorded yet: flights longer than one "
                              "second are saved when the vehicle is reset.")
                         : tr("%n session(s).", nullptr, static_cast<int>(sessions_.size())));
    updateButtons();
}

void HistoryPanel::fillAttempts() {
    const QSignalBlocker block(attemptsTable_);
    attemptsTable_->setRowCount(static_cast<int>(attempts_.size()));
    for (int row = 0; row < static_cast<int>(attempts_.size()); ++row) {
        const storage::MissionAttemptRecord& a = attempts_[static_cast<std::size_t>(row)];
        attemptsTable_->setItem(row, 0, cell(QString::fromStdString(a.missionKey)));
        attemptsTable_->setItem(row, 1, cell(shortTime(a.startedAt)));
        QString outcome = QString::fromStdString(a.outcome);
        if (!a.failureReason.empty()) {
            outcome += QStringLiteral(" — ") + QString::fromStdString(a.failureReason);
        }
        attemptsTable_->setItem(row, 2, cell(outcome));
        attemptsTable_->setItem(row, 3,
                                cell(QString(a.stars, QChar(0x2605)) +
                                     QString(3 - std::clamp(a.stars, 0, 3), QChar(0x2606))));
        attemptsTable_->setItem(row, 4, cell(QString::number(a.score, 'f', 0), true));
        attemptsTable_->setItem(row, 5, cell(duration(a.durationS), true));
        attemptsTable_->setItem(
            row, 6, cell(a.sessionId ? QStringLiteral("#%1").arg(*a.sessionId) : tr("—"), true));
    }
}

std::optional<std::int64_t> HistoryPanel::selectedSession() const {
    if (sessionsTable_ == nullptr) {
        return std::nullopt;
    }
    const QList<QTableWidgetItem*> items = sessionsTable_->selectedItems();
    if (items.isEmpty()) {
        return std::nullopt;
    }
    const int row = items.front()->row();
    if (row < 0 || row >= static_cast<int>(sessions_.size())) {
        return std::nullopt;
    }
    return sessions_[static_cast<std::size_t>(row)].id;
}

bool HistoryPanel::selectSession(std::int64_t sessionId) {
    for (int row = 0; row < static_cast<int>(sessions_.size()); ++row) {
        if (sessions_[static_cast<std::size_t>(row)].id == sessionId) {
            sessionsTable_->selectRow(row);
            return true;
        }
    }
    return false;
}

void HistoryPanel::loadTelemetry(std::int64_t sessionId) {
    const std::uint64_t request = ++telemetryRequest_;
    summary_->setText(tr("Loading…"));
    whenReady<storage::StorageResult<std::vector<storage::TelemetryRow>>>(
        storage_->submit([sessionId](storage::DbConnection& c) {
            return storage::RecordingStore(c).telemetry(sessionId);
        }),
        [this, request](storage::StorageResult<std::vector<storage::TelemetryRow>> rows) {
            if (request != telemetryRequest_) {
                return;  // Another session was selected meanwhile.
            }
            if (!rows) {
                summary_->setText(tr("Cannot read the telemetry: %1")
                                      .arg(QString::fromStdString(rows.error().message)));
                plot_->setSamples({});
                return;
            }
            plot_->setSamples(*rows);
            summary_->setText(rows->empty() ? tr("No telemetry for this session.")
                                            : tr("%n sample(s) at 20 Hz", nullptr,
                                                 static_cast<int>(rows->size())));
        });
}

void HistoryPanel::requestReplay() {
    const auto id = selectedSession();
    if (!id) {
        return;
    }
    replay_->setEnabled(false);
    whenReady<storage::StorageResult<std::optional<storage::ReplayRecord>>>(
        storage_->submit(
            [id = *id](storage::DbConnection& c) { return storage::RecordingStore(c).replay(id); }),
        [this](storage::StorageResult<std::optional<storage::ReplayRecord>> replay) {
            updateButtons();
            if (!replay || !*replay) {
                status_->setText(replay ? tr("This session has no replay.")
                                        : tr("Cannot read the replay: %1")
                                              .arg(QString::fromStdString(replay.error().message)));
                return;
            }
            emit replayRequested(**replay);
        });
}

void HistoryPanel::confirmRemove() {
    const auto id = selectedSession();
    if (!id) {
        return;
    }
    if (QMessageBox::question(this, tr("Delete session"),
                              tr("Delete this session with its telemetry and replay?")) !=
        QMessageBox::Yes) {
        return;
    }
    removeSession(*id);
}

void HistoryPanel::removeSession(std::int64_t sessionId) {
    if (storage_ == nullptr) {
        return;
    }
    whenReady<storage::StorageResult<bool>>(
        storage_->submit([sessionId](storage::DbConnection& c) {
            return storage::SessionStore(c).remove(sessionId);
        }),
        [this](storage::StorageResult<bool> removed) {
            if (!removed) {
                status_->setText(tr("Cannot delete the session: %1")
                                     .arg(QString::fromStdString(removed.error().message)));
            }
            refresh();
        });
}

void HistoryPanel::updateButtons() {
    if (storage_ == nullptr) {
        return;
    }
    const auto id = selectedSession();
    bool replayable = false;
    if (id) {
        for (const storage::SessionRecord& s : sessions_) {
            if (s.id == id) {
                replayable = s.hasReplay;
            }
        }
    }
    replay_->setEnabled(replayable && !simulation_->missionActive());
    stopReplay_->setEnabled(simulation_->replayActive());
    delete_->setEnabled(id.has_value());
}

}  // namespace ds::app
