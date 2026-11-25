// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/settings_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

#include "ds/app/settings_controller.hpp"
#include "ds/storage/storage_service.hpp"

namespace ds::app {
namespace {

QLabel* selectableLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

QString nativePath(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

}  // namespace

SettingsPanel::SettingsPanel(SettingsController& settings, QWidget* parent)
    : QWidget(parent), settings_(&settings) {
    // Display.
    units_ = new QComboBox;
    units_->setObjectName(QStringLiteral("UnitsCombo"));
    units_->addItem(tr("Metric (m, km/h)"), QVariant::fromValue(int(UnitSystem::Metric)));
    units_->addItem(tr("Imperial (ft, mph)"), QVariant::fromValue(int(UnitSystem::Imperial)));
    units_->setCurrentIndex(units_->findData(int(settings.units())));
    connect(units_, &QComboBox::currentIndexChanged, this, [this] {
        settings_->setUnits(static_cast<UnitSystem>(units_->currentData().toInt()));
    });
    connect(&settings, &SettingsController::unitsChanged, this, [this](UnitSystem units) {
        const QSignalBlocker blocker(units_);
        units_->setCurrentIndex(units_->findData(int(units)));
    });

    coordinates_ = new QComboBox;
    coordinates_->setObjectName(QStringLiteral("CoordinatesCombo"));
    coordinates_->addItem(tr("UTM"), QVariant::fromValue(int(CoordinateDisplay::Utm)));
    coordinates_->addItem(tr("UTM + latitude/longitude"),
                          QVariant::fromValue(int(CoordinateDisplay::UtmAndLatLon)));
    coordinates_->setCurrentIndex(coordinates_->findData(int(settings.coordinateDisplay())));
    connect(coordinates_, &QComboBox::currentIndexChanged, this, [this] {
        settings_->setCoordinateDisplay(
            static_cast<CoordinateDisplay>(coordinates_->currentData().toInt()));
    });
    connect(&settings, &SettingsController::coordinateDisplayChanged, this,
            [this](CoordinateDisplay display) {
                const QSignalBlocker blocker(coordinates_);
                coordinates_->setCurrentIndex(coordinates_->findData(int(display)));
            });

    // Chase camera insets.
    chaseTopView_ = new QCheckBox(tr("Show &top view"));
    chaseTopView_->setObjectName(QStringLiteral("ChaseTopViewCheck"));
    chaseTopView_->setToolTip(
        tr("Chase camera: the inset in the bottom-left corner, the vehicle seen from above "
           "with north up."));
    chaseTopView_->setChecked(settings.chaseTopView());
    connect(chaseTopView_, &QCheckBox::toggled, this,
            [this](bool on) { settings_->setChaseTopView(on); });
    chaseBehindView_ = new QCheckBox(tr("Show &behind view"));
    chaseBehindView_->setObjectName(QStringLiteral("ChaseBehindViewCheck"));
    chaseBehindView_->setToolTip(
        tr("Chase camera: the inset in the bottom-right corner, the view from right behind "
           "the vehicle's nose."));
    chaseBehindView_->setChecked(settings.chaseBehindView());
    connect(chaseBehindView_, &QCheckBox::toggled, this,
            [this](bool on) { settings_->setChaseBehindView(on); });
    connect(&settings, &SettingsController::chaseInsetsChanged, this,
            [this](bool top, bool behind) {
                const QSignalBlocker topBlocker(chaseTopView_);
                const QSignalBlocker behindBlocker(chaseBehindView_);
                chaseTopView_->setChecked(top);
                chaseBehindView_->setChecked(behind);
            });

    // Flight rules (FR-MIS-06d).
    enforceRules_ = new QCheckBox(tr("&End free flights that break a flight rule"));
    enforceRules_->setObjectName(QStringLiteral("EnforceRulesCheck"));
    enforceRules_->setToolTip(
        tr("Free mode always shows warnings for the 120 m height limit and visual line of "
           "sight. When checked, a rule broken for longer than allowed also ends the flight, "
           "as in a mission."));
    enforceRules_->setChecked(settings.enforceFreeFlightRules());
    connect(enforceRules_, &QCheckBox::toggled, this,
            [this](bool on) { settings_->setEnforceFreeFlightRules(on); });
    connect(&settings, &SettingsController::enforceFreeFlightRulesChanged, this, [this](bool on) {
        const QSignalBlocker blocker(enforceRules_);
        enforceRules_->setChecked(on);
    });
    auto* rulesGroup = new QGroupBox(tr("Flight rules"));
    auto* rulesLayout = new QVBoxLayout(rulesGroup);
    rulesLayout->addWidget(enforceRules_);

    auto* displayGroup = new QGroupBox(tr("Display"));
    auto* displayForm = new QFormLayout(displayGroup);
    displayForm->addRow(tr("&Units:"), units_);
    displayForm->addRow(tr("&Coordinates:"), coordinates_);
    displayForm->addRow(tr("Chase camera:"), chaseTopView_);
    displayForm->addRow(QString(), chaseBehindView_);

    // Data.
    const storage::StorageInfo& info = settings.storageInfo();
    auto* dataGroup = new QGroupBox(tr("Data"));
    auto* dataForm = new QFormLayout(dataGroup);
    if (info.inMemory) {
        database_ = selectableLabel(tr("Temporary (in memory) — nothing is saved"));
    } else {
        database_ = selectableLabel(nativePath(info.databaseFile));
    }
    database_->setObjectName(QStringLiteral("DatabaseLabel"));
    dataForm->addRow(tr("Database:"), database_);
    auto* schema = selectableLabel(QString::number(info.migration.toVersion));
    schema->setObjectName(QStringLiteral("SchemaLabel"));
    dataForm->addRow(tr("Schema version:"), schema);
    if (info.migration.backup) {
        dataForm->addRow(tr("Pre-upgrade backup:"),
                         selectableLabel(nativePath(*info.migration.backup)));
    }
    openFolder_ = new QPushButton(tr("&Open Data Folder"));
    openFolder_->setObjectName(QStringLiteral("OpenDataFolderButton"));
    openFolder_->setEnabled(!info.inMemory);
    connect(openFolder_, &QPushButton::clicked, this, &SettingsPanel::openDataFolder);
    dataForm->addRow(QString(), openFolder_);

    auto* later = new QLabel(tr("Graphics settings arrive in a later milestone."));
    later->setWordWrap(true);
    later->setEnabled(false);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(displayGroup);
    layout->addWidget(rulesGroup);
    layout->addWidget(dataGroup);
    layout->addWidget(later);
    layout->addStretch(1);
}

void SettingsPanel::openDataFolder() {
    const auto folder = settings_->storageInfo().databaseFile.parent_path();
    QDesktopServices::openUrl(QUrl::fromLocalFile(nativePath(folder)));
}

}  // namespace ds::app
