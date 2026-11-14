// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/vehicle_panel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <string_view>

#include "ds/app/simulation_controller.hpp"
#include "ds/physics/aerial_profile.hpp"

namespace ds::app {
namespace {

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace

VehiclePanel::VehiclePanel(SimulationController& simulation, QWidget* parent)
    : QWidget(parent), simulation_(&simulation) {
    buildUi();
    connect(simulation_, &SimulationController::profilesChanged, this,
            &VehiclePanel::reloadProfiles);
    connect(simulation_, &SimulationController::profileChanged, this, &VehiclePanel::refresh);
    refresh();
}

void VehiclePanel::buildUi() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    auto* profileBox = new QGroupBox(tr("Profile"));
    auto* profileLayout = new QVBoxLayout(profileBox);
    profile_ = new QComboBox;
    profile_->setObjectName(QStringLiteral("ProfileCombo"));
    connect(profile_, &QComboBox::activated, this, [this](int index) {
        if (auto selected = simulation_->selectProfile(profile_->itemData(index).toString());
            !selected) {
            showError(QString::fromStdString(selected.error().message));
            refresh();
        }
    });
    profileLayout->addWidget(profile_);
    description_ = new QLabel;
    description_->setObjectName(QStringLiteral("ProfileDescription"));
    description_->setWordWrap(true);
    profileLayout->addWidget(description_);
    auto* profileButtons = new QHBoxLayout;
    saveAs_ = new QPushButton(tr("Save As…"));
    saveAs_->setObjectName(QStringLiteral("SaveProfileButton"));
    delete_ = new QPushButton(tr("Delete"));
    delete_->setObjectName(QStringLiteral("DeleteProfileButton"));
    profileButtons->addWidget(saveAs_);
    profileButtons->addWidget(delete_);
    profileButtons->addStretch();
    profileLayout->addLayout(profileButtons);
    connect(saveAs_, &QPushButton::clicked, this, &VehiclePanel::saveAs);
    connect(delete_, &QPushButton::clicked, this, &VehiclePanel::remove);
    layout->addWidget(profileBox);

    auto* figuresBox = new QGroupBox(tr("Performance (computed)"));
    auto* figuresLayout = new QVBoxLayout(figuresBox);
    figures_ = new QLabel;
    figures_->setObjectName(QStringLiteral("FiguresLabel"));
    figures_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    figures_->setWordWrap(true);
    figuresLayout->addWidget(figures_);
    layout->addWidget(figuresBox);

    // Parameter editor, one group box per parameter group, in table order.
    std::map<std::string_view, QFormLayout*> forms;
    for (const physics::ParamDescriptor& d : physics::aerialParameters()) {
        QFormLayout*& form = forms[d.group];
        if (form == nullptr) {
            auto* box = new QGroupBox(fromView(d.group));
            form = new QFormLayout(box);
            layout->addWidget(box);
        }
        auto* spin = new QDoubleSpinBox;
        spin->setObjectName(QStringLiteral("Param") + fromView(d.pointer));
        spin->setRange(d.min, d.max);
        spin->setDecimals(d.kind == physics::ParamKind::Integer ? 0 : d.decimals);
        // One step changes the second-to-last shown digit (e.g. 0.01 for 3 decimals).
        const int stepDigits = d.decimals >= 2 ? d.decimals - 1 : d.decimals;
        spin->setSingleStep(d.kind == physics::ParamKind::Integer ? 1.0
                                                                  : std::pow(10.0, -stepDigits));
        if (!d.unit.empty()) {
            spin->setSuffix(QLatin1Char(' ') + fromView(d.unit));
        }
        spin->setKeyboardTracking(false);
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
            edited_ = true;
            updateButtons();
        });
        form->addRow(fromView(d.label) + QLatin1Char(':'), spin);
        params_.push_back(spin);
    }

    auto* editBox = new QHBoxLayout;
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("ProfileStatus"));
    apply_ = new QPushButton(tr("Apply"));
    apply_->setObjectName(QStringLiteral("ApplyProfileButton"));
    apply_->setToolTip(tr("Fly with these parameters (the vehicle is reset)."));
    revert_ = new QPushButton(tr("Revert"));
    revert_->setObjectName(QStringLiteral("RevertProfileButton"));
    editBox->addWidget(status_, 1);
    editBox->addWidget(revert_);
    editBox->addWidget(apply_);
    connect(apply_, &QPushButton::clicked, this, &VehiclePanel::apply);
    connect(revert_, &QPushButton::clicked, this, [this] {
        const QString key = QString::fromStdString(simulation_->profile().key);
        if (auto selected = simulation_->selectProfile(key); !selected) {
            showError(QString::fromStdString(selected.error().message));
        }
    });
    layout->addLayout(editBox);
    layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

void VehiclePanel::refresh() {
    reloadProfiles();
    loadParameters();
    updateFigures();
}

void VehiclePanel::reloadProfiles() {
    const QSignalBlocker block(profile_);
    profile_->clear();
    const QString current = QString::fromStdString(simulation_->profile().key);
    for (const VehicleProfileInfo& p : simulation_->profiles()) {
        profile_->addItem(p.builtIn ? p.name : tr("%1 (user)").arg(p.name), p.key);
        if (p.key == current) {
            profile_->setCurrentIndex(profile_->count() - 1);
        }
    }
    updateButtons();
}

void VehiclePanel::loadParameters() {
    const physics::AerialProfile& p = simulation_->profile();
    description_->setText(QString::fromStdString(p.description));
    const auto descriptors = physics::aerialParameters();
    for (std::size_t i = 0; i < params_.size(); ++i) {
        const QSignalBlocker block(params_[i]);
        params_[i]->setValue(descriptors[i].get(p));
    }
    edited_ = false;
    updateButtons();
}

void VehiclePanel::updateFigures() {
    const physics::AerialProfile& p = simulation_->profile();
    const physics::AerialStats s = physics::computeAerialStats(p);
    const int windLevel = simulation_->windTable().highestLevelWithGustsAtMost(s.maxWindMs);
    QString text = tr("Hover: %1 % throttle, %2 A, %3 W\n"
                      "Thrust-to-weight: %4 (%5 N)\n"
                      "Hover endurance: %6 min\n"
                      "Top level speed: %7 m/s (%8 km/h)\n"
                      "Holds position in wind up to %9 m/s (tilt %10°): wind level %11")
                       .arg(100.0 * s.hoverThrottle, 0, 'f', 0)
                       .arg(s.hoverCurrentA, 0, 'f', 1)
                       .arg(s.hoverPowerW, 0, 'f', 0)
                       .arg(s.thrustToWeight, 0, 'f', 1)
                       .arg(s.maxThrustN, 0, 'f', 1)
                       .arg(s.hoverEnduranceMin, 0, 'f', 1)
                       .arg(s.maxLevelSpeedMs, 0, 'f', 1)
                       .arg(3.6 * s.maxLevelSpeedMs, 0, 'f', 0)
                       .arg(s.maxWindMs, 0, 'f', 1)
                       .arg(s.maxWindTiltDeg, 0, 'f', 0)
                       .arg(windLevel);
    if (p.reference && !simulation_->profileModified() && !edited_) {
        const physics::ReferenceFigures& r = *p.reference;
        text += tr("\n\nReference (%1): hover %2 %, T/W %3, %4 min, %5 m/s")
                    .arg(QString::fromStdString(r.source))
                    .arg(100.0 * r.hoverThrottle, 0, 'f', 0)
                    .arg(r.thrustToWeight, 0, 'f', 1)
                    .arg(r.hoverEnduranceMin, 0, 'f', 1)
                    .arg(r.maxLevelSpeedMs, 0, 'f', 1);
    }
    figures_->setText(text);
}

void VehiclePanel::updateButtons() {
    const QString key = QString::fromStdString(simulation_->profile().key);
    const bool modified = simulation_->profileModified();
    apply_->setEnabled(edited_);
    revert_->setEnabled(edited_ || modified);
    delete_->setEnabled(key.startsWith(QLatin1String("user.")) && !modified);
    if (edited_) {
        status_->setText(tr("Edited: Apply to fly with it."));
    } else if (modified) {
        status_->setText(tr("Flying edited parameters (not saved)."));
    } else {
        status_->clear();
    }
}

void VehiclePanel::apply() {
    physics::AerialProfile p = simulation_->profile();
    const auto descriptors = physics::aerialParameters();
    for (std::size_t i = 0; i < params_.size(); ++i) {
        descriptors[i].set(p, params_[i]->value());
    }
    if (auto applied = simulation_->applyProfile(p); !applied) {
        showError(QString::fromStdString(applied.error().message));
    }
}

void VehiclePanel::saveAs() {
    if (edited_) {
        apply();
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Vehicle Profile"), tr("Profile name:"), QLineEdit::Normal,
        tr("%1 (copy)").arg(QString::fromStdString(simulation_->profile().name)), &ok);
    if (!ok) {
        return;
    }
    if (auto saved = simulation_->saveProfileAs(name); !saved) {
        showError(QString::fromStdString(saved.error().message));
    }
}

void VehiclePanel::remove() {
    const QString key = QString::fromStdString(simulation_->profile().key);
    if (QMessageBox::question(this, tr("Delete Vehicle Profile"),
                              tr("Delete the profile “%1”?")
                                  .arg(QString::fromStdString(simulation_->profile().name))) !=
        QMessageBox::Yes) {
        return;
    }
    if (auto removed = simulation_->deleteProfile(key); !removed) {
        showError(QString::fromStdString(removed.error().message));
    }
}

void VehiclePanel::showError(const QString& message) {
    QMessageBox::warning(this, tr("Vehicle Profile"), message);
}

}  // namespace ds::app
