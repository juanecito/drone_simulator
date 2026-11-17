// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/missions_panel.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>

#include "ds/app/missions_controller.hpp"
#include "ds/astro/time.hpp"

namespace ds::app {
namespace {

enum Column { kTitle, kType, kStars, kBest, kStatus, kColumns };
constexpr int kKeyRole = Qt::UserRole;

QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString html(std::string_view text) { return qs(text).toHtmlEscaped(); }

QString tr(const char* text) { return QCoreApplication::translate("ds::app::MissionsPanel", text); }

QString statusText(const MissionRow& row) {
    switch (row.status) {
        case MissionStatus::Available: return tr("Available");
        case MissionStatus::Locked: return tr("Locked");
        case MissionStatus::Unavailable: return tr("Unavailable");
        case MissionStatus::Invalid: return tr("Invalid");
    }
    return {};
}

QString number(double value) { return QString::number(value, 'g', 6); }

QString terrainText(const missions::TerrainSpec& t) {
    QString text;
    switch (t.source) {
        case missions::TerrainSource::Procedural:
            text = t.preset.empty()
                       ? tr("Procedural (seed %1)").arg(t.seed)
                       : tr("Procedural, %1 (seed %2)").arg(html(t.preset)).arg(t.seed);
            break;
        case missions::TerrainSource::Online:
            text = tr("Online elevation: %1").arg(html(t.elevationProvider));
            if (!t.dataset.empty()) {
                text += QStringLiteral(" (%1)").arg(html(t.dataset));
            }
            break;
        case missions::TerrainSource::File: text = tr("Elevation files"); break;
    }
    if (t.extent) {
        text += tr(", %1 × %2 km")
                    .arg(number(t.extent->widthM / 1000.0), number(t.extent->lengthM / 1000.0));
    }
    return text;
}

QString timeText(const missions::EnvironmentSpec& e) {
    if (!e.timeZone.empty()) {
        if (const auto local = astro::toLocalTime(e.utc, e.timeZone)) {
            const auto minutes = std::chrono::floor<std::chrono::minutes>(local->time);
            return QStringLiteral("%1 %2 (%3)")
                .arg(qs(std::format("{:%Y-%m-%d %H:%M}", minutes)), html(local->abbreviation),
                     html(e.timeZone));
        }
    }
    const auto minutes = std::chrono::floor<std::chrono::minutes>(e.utc);
    return qs(std::format("{:%Y-%m-%d %H:%M} UTC", minutes));
}

QString weatherText(const missions::WeatherSpec& w) {
    QString text = html(w.preset);
    text.replace(QLatin1Char('-'), QLatin1Char(' '));
    if (w.temperatureC) {
        text += QStringLiteral(", %1 °C").arg(number(*w.temperatureC));
    }
    text += tr(", wind level %1").arg(w.windLevel);
    if (w.windLevel > 0) {
        text += tr(" from %1°").arg(number(w.windFromDeg));
    }
    return text;
}

QString starCondition(const missions::StarThreshold& s) {
    QStringList parts;
    if (s.maxTimeS) {
        parts << tr("time ≤ %1").arg(durationText(*s.maxTimeS));
    }
    if (s.maxErrorM) {
        parts << tr("error ≤ %1 m").arg(number(*s.maxErrorM));
    }
    if (s.minBatteryPct) {
        parts << tr("battery ≥ %1 %").arg(number(*s.minBatteryPct));
    }
    if (s.minScore) {
        parts << tr("score ≥ %1").arg(number(*s.minScore));
    }
    return parts.join(QStringLiteral(", "));
}

QString list(const QStringList& items, const char* tag = "ul") {
    QString out = QStringLiteral("<%1>").arg(QLatin1StringView(tag));
    for (const QString& item : items) {
        out += QStringLiteral("<li>%1</li>").arg(item);
    }
    return out + QStringLiteral("</%1>").arg(QLatin1StringView(tag));
}

}  // namespace

QString starsText(int stars) {
    if (stars <= 0) {
        return QStringLiteral("—");
    }
    return QString(std::min(stars, 3), QChar(0x2605)) + QString(3 - std::min(stars, 3), QChar(0x2606));
}

QString durationText(double seconds) {
    const double tenths = std::round(seconds * 10.0);
    const int minutes = static_cast<int>(tenths / 600.0);
    const double rest = (tenths - minutes * 600.0) / 10.0;
    return QStringLiteral("%1:%2").arg(minutes).arg(rest, 4, 'f', 1, QLatin1Char('0'));
}

QString missionDetailsHtml(const MissionRow& row, const missions::CatalogEntry& entry) {
    QString out = QStringLiteral("<h3>%1</h3>").arg(html(row.title));
    const QString file = html(entry.load.file.string());
    if (row.status == MissionStatus::Invalid) {
        QStringList errors;
        for (const missions::MissionIssue& e : entry.load.errors) {
            errors << html(e.text());
        }
        out += QStringLiteral("<p><b>%1</b></p>").arg(tr("This mission file has errors and cannot be flown:"));
        out += list(errors);
        out += QStringLiteral("<p><small>%1</small></p>").arg(file);
        return out;
    }
    const missions::MissionDefinition& m = *entry.load.definition;
    out += QStringLiteral("<p><i>%1 · %2 · %3 %4</i></p>")
               .arg(row.ground ? tr("Ground vehicle") : tr("Aerial"),
                    row.builtIn ? tr("Built-in mission") : tr("Your mission"), tr("difficulty"),
                    QString(m.difficulty, QChar(0x25CF)) + QString(5 - m.difficulty, QChar(0x25CB)));
    if (row.status == MissionStatus::Locked) {
        out += QStringLiteral("<p><b>%1</b></p>")
                   .arg(row.statusDetail.empty() ? tr("Locked.")
                                                 : tr("Locked: needs %1.").arg(html(row.statusDetail)));
    } else if (row.status == MissionStatus::Unavailable) {
        QStringList reasons;
        for (const std::string& r : entry.load.unavailable) {
            reasons << html(r);
        }
        if (reasons.isEmpty() && !row.statusDetail.empty()) {
            reasons << html(row.statusDetail);
        }
        out += QStringLiteral("<p><b>%1</b></p>").arg(tr("Not playable yet:")) + list(reasons);
    }
    if (!m.briefing.empty()) {
        out += QStringLiteral("<p>%1</p>").arg(html(m.briefing));
    }

    QStringList objectives;
    for (const missions::ObjectiveSpec& o : m.objectives) {
        QString text = html(missions::objectiveTitle(m, o));
        if (o.optional) {
            text += QStringLiteral(" <i>%1</i>").arg(tr("(optional, +%1 points)").arg(number(o.bonus)));
        }
        objectives << text;
    }
    out += QStringLiteral("<p><b>%1</b>%2</p>")
               .arg(tr("Objectives"), m.objectiveOrder == missions::ObjectiveOrder::Sequential
                                          ? QStringLiteral(" (%1)").arg(tr("in order"))
                                          : QString());
    out += list(objectives,
                m.objectiveOrder == missions::ObjectiveOrder::Sequential ? "ol" : "ul");

    QStringList conditions;
    conditions << tr("Terrain: %1").arg(terrainText(m.terrain));
    conditions << tr("Time: %1").arg(timeText(m.environment));
    conditions << tr("Weather: %1").arg(weatherText(m.environment.weather));
    QString vehicle = m.vehicle.profile.empty() ? tr("custom profile") : html(m.vehicle.profile);
    if (m.vehicle.allowProfileChange) {
        vehicle += tr(" (may be changed)");
    }
    conditions << tr("Vehicle: %1").arg(vehicle);
    out += QStringLiteral("<p><b>%1</b></p>").arg(tr("Conditions")) + list(conditions);

    QStringList rules;
    if (!row.ground) {
        rules << (m.rules.maxAltitudeAglM
                      ? tr("Height limit %1 m above the ground").arg(number(*m.rules.maxAltitudeAglM))
                      : tr("No height limit"));
    }
    if (m.rules.vlos.enabled) {
        rules << tr("Keep it in sight: at most %1 m away, hidden for at most %2 s")
                     .arg(number(m.rules.vlos.maxDistanceM), number(m.rules.vlos.maxHiddenS));
    }
    rules << (m.rules.timeLimitS ? tr("Time limit %1").arg(durationText(*m.rules.timeLimitS))
                                 : tr("No time limit"));
    if (m.rules.minBatteryPct > 0.0) {
        rules << tr("Keep the battery above %1 %").arg(number(m.rules.minBatteryPct));
    }
    // Keyboard flying is a special rule (listed below) when allowed.
    if (!m.rules.allowKeyboard) {
        rules << tr("Controller or transmitter required");
    }
    for (const std::string& r : m.rules.specialRules) {
        rules << QStringLiteral("<b>%1</b>").arg(html(r));
    }
    out += QStringLiteral("<p><b>%1</b></p>").arg(tr("Rules")) + list(rules);

    QStringList stars{tr("★ Complete the mission")};
    for (std::size_t i = 0; i < m.scoring.stars.size() && i < 2; ++i) {
        stars << QStringLiteral("%1 %2").arg(starsText(static_cast<int>(i) + 2).remove(QChar(0x2606)),
                                             starCondition(m.scoring.stars[i]));
    }
    out += QStringLiteral("<p><b>%1</b></p>").arg(tr("Stars")) + list(stars);

    if (row.attempts > 0) {
        QString progress = row.attempts == 1 ? tr("1 attempt") : tr("%1 attempts").arg(row.attempts);
        progress += tr(", best %1").arg(starsText(row.bestStars));
        if (row.bestTimeS) {
            progress += tr(", fastest %1").arg(durationText(*row.bestTimeS));
        }
        out += QStringLiteral("<p><b>%1</b> %2</p>").arg(tr("Your results:"), progress);
    }
    if (!entry.load.warnings.empty()) {
        QStringList warnings;
        for (const missions::MissionIssue& w : entry.load.warnings) {
            warnings << html(w.text());
        }
        out += QStringLiteral("<p><b>%1</b></p>").arg(tr("Warnings")) + list(warnings);
    }
    out += QStringLiteral("<p><small>%1</small></p>").arg(file);
    return out;
}

MissionsPanel::MissionsPanel(MissionsController& missions, QWidget* parent)
    : QWidget(parent), missions_(&missions) {
    auto* layout = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    summary_ = new QLabel;
    summary_->setObjectName(QStringLiteral("MissionSummary"));
    summary_->setWordWrap(true);
    top->addWidget(summary_, 1);
    reload_ = new QPushButton(tr("Reload"));
    reload_->setObjectName(QStringLiteral("ReloadMissions"));
    reload_->setToolTip(tr("Scan the mission folders again"));
    connect(reload_, &QPushButton::clicked, missions_, &MissionsController::reload);
    top->addWidget(reload_);
    openFolder_ = new QPushButton(tr("Open Folder"));
    openFolder_->setObjectName(QStringLiteral("OpenMissionsFolder"));
    openFolder_->setEnabled(!missions_->folders().userDir.empty());
    openFolder_->setToolTip(tr("Open your mission folder %1: every mission file (.json) put there "
                               "is listed here")
                                .arg(qs(missions_->folders().userDir.string())));
    connect(openFolder_, &QPushButton::clicked, this, [this] {
        const auto& dir = missions_->folders().userDir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        QDesktopServices::openUrl(QUrl::fromLocalFile(qs(dir.string())));
    });
    top->addWidget(openFolder_);
    layout->addLayout(top);

    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("MissionTree"));
    tree_->setColumnCount(kColumns);
    tree_->setHeaderLabels({tr("Mission"), tr("Type"), tr("Stars"), tr("Best"), tr("Status")});
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(kTitle, QHeaderView::Stretch);
    for (int c = kType; c < kColumns; ++c) {
        tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    }
    connect(tree_, &QTreeWidget::currentItemChanged, this, &MissionsPanel::showDetails);

    details_ = new QTextBrowser;
    details_->setObjectName(QStringLiteral("MissionDetails"));
    details_->setOpenLinks(false);

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(tree_);
    splitter->addWidget(details_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    start_ = new QPushButton(tr("Start Mission"));
    start_->setObjectName(QStringLiteral("StartMission"));
    start_->setEnabled(false);
    start_->setToolTip(tr("Open the briefing of the selected mission"));
    connect(start_, &QPushButton::clicked, this, [this] {
        if (const std::string key = selectedMission(); !key.empty() && start_->isEnabled()) {
            emit startRequested(key);
        }
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const std::string key = item->data(kTitle, kKeyRole).toString().toStdString();
        if (!key.empty() && key == selectedMission() && start_->isEnabled()) {
            emit startRequested(key);
        }
    });
    layout->addWidget(start_);

    connect(missions_, &MissionsController::missionsChanged, this, &MissionsPanel::rebuild);
    rebuild();
}

std::string MissionsPanel::selectedMission() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    return item != nullptr ? item->data(kTitle, kKeyRole).toString().toStdString() : std::string();
}

bool MissionsPanel::selectMission(const std::string& key) {
    for (QTreeWidgetItemIterator it(tree_); *it != nullptr; ++it) {
        if ((*it)->data(kTitle, kKeyRole).toString().toStdString() == key && !key.empty()) {
            tree_->setCurrentItem(*it);
            return true;
        }
    }
    return false;
}

void MissionsPanel::rebuild() {
    const std::string selected = selectedMission();
    tree_->clear();
    const auto& rows = missions_->missions();
    if (!missions_->loadError().isEmpty()) {
        summary_->setText(tr("Missions cannot be loaded: %1").arg(missions_->loadError()));
    } else {
        const auto playable = std::ranges::count(rows, MissionStatus::Available, &MissionRow::status);
        const auto invalid = std::ranges::count(rows, MissionStatus::Invalid, &MissionRow::status);
        QString text = rows.size() == 1 ? tr("1 mission") : tr("%1 missions").arg(rows.size());
        text += tr(", %1 available").arg(playable);
        if (invalid > 0) {
            text += tr(", %1 invalid").arg(invalid);
        }
        summary_->setText(text);
    }

    std::map<QString, QTreeWidgetItem*> groups;
    std::vector<QTreeWidgetItem*> order;
    const QColor muted = palette().color(QPalette::Disabled, QPalette::Text);
    for (const MissionRow& row : rows) {
        const QString group = row.status == MissionStatus::Invalid ? tr("Invalid missions")
                                                                   : chapterTitle(row.chapter);
        QTreeWidgetItem*& parent = groups[group];
        if (parent == nullptr) {
            parent = new QTreeWidgetItem({group});
            parent->setFlags(Qt::ItemIsEnabled);
            QFont bold = parent->font(kTitle);
            bold.setBold(true);
            parent->setFont(kTitle, bold);
            order.push_back(parent);
        }
        auto* item = new QTreeWidgetItem(parent);
        item->setText(kTitle, qs(row.title));
        item->setData(kTitle, kKeyRole, qs(row.key));
        if (row.status != MissionStatus::Invalid) {
            item->setText(kType, row.ground ? tr("Ground") : tr("Aerial"));
        }
        item->setText(kStars, starsText(row.bestStars));
        item->setText(kBest, row.bestTimeS ? durationText(*row.bestTimeS) : QStringLiteral("—"));
        item->setText(kStatus, statusText(row));
        const QString tip = row.statusDetail.empty()
                                ? qs(row.key)
                                : QStringLiteral("%1\n%2").arg(qs(row.key), qs(row.statusDetail));
        for (int c = 0; c < kColumns; ++c) {
            item->setToolTip(c, tip);
            if (row.status != MissionStatus::Available) {
                item->setForeground(c, muted);
            }
        }
    }
    tree_->addTopLevelItems({order.begin(), order.end()});
    tree_->expandAll();
    if (!selectMission(selected)) {
        // First playable mission, else the first one.
        const auto first = std::ranges::find(rows, MissionStatus::Available, &MissionRow::status);
        if (first != rows.end()) {
            selectMission(first->key);
        } else if (!rows.empty()) {
            selectMission(rows.front().key);
        }
    }
    showDetails();
}

void MissionsPanel::setMissionActive(bool active) {
    missionActive_ = active;
    updateStart();
}

void MissionsPanel::updateStart() {
    const MissionRow* row = missions_->find(selectedMission());
    start_->setEnabled(!missionActive_ && row != nullptr &&
                       row->status == MissionStatus::Available);
}

void MissionsPanel::showDetails() {
    updateStart();
    const MissionRow* row = missions_->find(selectedMission());
    if (row != nullptr) {
        emit missionSelected(row->key);
    }
    if (row == nullptr) {
        details_->setHtml(
            QStringLiteral("<p>%1</p>")
                .arg(tr("No mission selected. Put mission files (.json) in your mission folder "
                        "and press Reload.")));
        return;
    }
    details_->setHtml(missionDetailsHtml(*row, missions_->entry(*row)));
}

}  // namespace ds::app
