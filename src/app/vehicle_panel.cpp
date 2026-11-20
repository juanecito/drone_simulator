// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/vehicle_panel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
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
#include "ds/physics/ground_profile.hpp"

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
    connect(simulation_, &SimulationController::missionActiveChanged, this,
            &VehiclePanel::updateButtons);
    refresh();
}

void VehiclePanel::buildEditor(QWidget* parent, bool ground) {
    auto* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(0, 0, 0, 0);
    // One group box per parameter group, in table order.
    std::map<std::string_view, QFormLayout*> forms;
    auto add = [&](const auto& d) {
        QFormLayout*& form = forms[d.group];
        if (form == nullptr) {
            auto* box = new QGroupBox(fromView(d.group));
            form = new QFormLayout(box);
            layout->addWidget(box);
        }
        auto* spin = new QDoubleSpinBox;
        spin->setObjectName((ground ? QStringLiteral("GroundParam") : QStringLiteral("Param")) +
                            fromView(d.pointer));
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
        (ground ? groundParams_ : params_).push_back(spin);
    };
    if (ground) {
        for (const physics::GroundParamDescriptor& d : physics::groundParameters()) {
            add(d);
        }
    } else {
        for (const physics::ParamDescriptor& d : physics::aerialParameters()) {
            add(d);
        }
    }
}

void VehiclePanel::buildUi() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    auto* profileBox = new QGroupBox(tr("Profile"));
    auto* profileLayout = new QVBoxLayout(profileBox);
    auto* typeRow = new QFormLayout;
    type_ = new QComboBox;
    type_->setObjectName(QStringLiteral("VehicleTypeCombo"));
    type_->addItem(tr("Aerial drone"), static_cast<int>(physics::VehicleType::Aerial));
    type_->addItem(tr("Ground drone"), static_cast<int>(physics::VehicleType::Ground));
    connect(type_, &QComboBox::activated, this, [this](int index) {
        const auto type = static_cast<physics::VehicleType>(type_->itemData(index).toInt());
        if (auto selected = simulation_->selectVehicleType(type); !selected) {
            showError(QString::fromStdString(selected.error().message));
            refresh();
        }
    });
    typeRow->addRow(tr("Type:"), type_);
    profileLayout->addLayout(typeRow);
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
    // Model file drawn instead of the built-in body (FR-GFX-12).
    auto* modelRow = new QHBoxLayout;
    modelRow->addWidget(new QLabel(tr("Model:")));
    model_ = new QLabel;
    model_->setObjectName(QStringLiteral("ModelFileLabel"));
    model_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    modelRow->addWidget(model_, 1);
    auto* chooseModel = new QPushButton(tr("Choose…"));
    chooseModel->setObjectName(QStringLiteral("ChooseModelButton"));
    chooseModel->setToolTip(
        tr("Draw the body from a glTF 2.0 (.gltf, .glb) or OBJ file, fitted "
           "to the built-in body; wheels, propellers and lights stay."));
    builtInModel_ = new QPushButton(tr("Built-in"));
    builtInModel_->setObjectName(QStringLiteral("BuiltInModelButton"));
    modelRow->addWidget(chooseModel);
    modelRow->addWidget(builtInModel_);
    connect(chooseModel, &QPushButton::clicked, this, &VehiclePanel::chooseModel);
    connect(builtInModel_, &QPushButton::clicked, this, [this] { setModelFile({}, true); });
    profileLayout->addLayout(modelRow);
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

    // Parameter editors: one per vehicle type, the one of the flown type shown.
    aerialEditor_ = new QWidget;
    aerialEditor_->setObjectName(QStringLiteral("AerialEditor"));
    buildEditor(aerialEditor_, false);
    layout->addWidget(aerialEditor_);
    groundEditor_ = new QWidget;
    groundEditor_->setObjectName(QStringLiteral("GroundEditor"));
    buildEditor(groundEditor_, true);
    layout->addWidget(groundEditor_);

    auto* editBox = new QHBoxLayout;
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("ProfileStatus"));
    apply_ = new QPushButton(tr("Apply"));
    apply_->setObjectName(QStringLiteral("ApplyProfileButton"));
    apply_->setToolTip(tr("Fly or drive with these parameters (the vehicle is reset)."));
    revert_ = new QPushButton(tr("Revert"));
    revert_->setObjectName(QStringLiteral("RevertProfileButton"));
    editBox->addWidget(status_, 1);
    editBox->addWidget(revert_);
    editBox->addWidget(apply_);
    connect(apply_, &QPushButton::clicked, this, &VehiclePanel::apply);
    connect(revert_, &QPushButton::clicked, this, [this] {
        if (auto selected = simulation_->selectProfile(simulation_->profileKey()); !selected) {
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
    const physics::VehicleType type = simulation_->vehicleType();
    {
        const QSignalBlocker block(type_);
        type_->setCurrentIndex(type_->findData(static_cast<int>(type)));
    }
    const QSignalBlocker block(profile_);
    profile_->clear();
    const QString current = simulation_->profileKey();
    for (const VehicleProfileInfo& p : simulation_->profiles(type)) {
        profile_->addItem(p.builtIn ? p.name : tr("%1 (user)").arg(p.name), p.key);
        if (p.key == current) {
            profile_->setCurrentIndex(profile_->count() - 1);
        }
    }
    updateButtons();
}

void VehiclePanel::loadParameters() {
    const bool ground = simulation_->vehicleType() == physics::VehicleType::Ground;
    aerialEditor_->setVisible(!ground);
    groundEditor_->setVisible(ground);
    if (const physics::AerialProfile* p = simulation_->aerialProfile()) {
        description_->setText(QString::fromStdString(p->description));
        setModelFile(p->modelFile, false);
        const auto descriptors = physics::aerialParameters();
        for (std::size_t i = 0; i < params_.size(); ++i) {
            const QSignalBlocker block(params_[i]);
            params_[i]->setValue(descriptors[i].get(*p));
        }
    } else if (const physics::GroundProfile* g = simulation_->groundProfile()) {
        description_->setText(QString::fromStdString(g->description));
        setModelFile(g->modelFile, false);
        const auto descriptors = physics::groundParameters();
        for (std::size_t i = 0; i < groundParams_.size(); ++i) {
            const QSignalBlocker block(groundParams_[i]);
            groundParams_[i]->setValue(descriptors[i].get(*g));
        }
    }
    edited_ = false;
    updateButtons();
}

void VehiclePanel::updateFigures() {
    if (const physics::GroundProfile* g = simulation_->groundProfile()) {
        const physics::GroundStats s = physics::computeGroundStats(*g);
        QString text = tr("Top speed: %1 m/s (%2 km/h)\n"
                          "0 → 90 % of top speed: %3 s\n"
                          "Stall torque per wheel: %4 N·m\n"
                          "Static rollover: %5° sideways, %6° front/back\n"
                          "Range: %7 km at %8 m/s on grass (%9 A, %10 min)")
                           .arg(s.topSpeedMs, 0, 'f', 2)
                           .arg(3.6 * s.topSpeedMs, 0, 'f', 1)
                           .arg(s.accelerationTimeS, 0, 'f', 2)
                           .arg(s.stallTorqueNm, 0, 'f', 2)
                           .arg(s.rolloverSideDeg, 0, 'f', 0)
                           .arg(s.rolloverPitchDeg, 0, 'f', 0)
                           .arg(s.rangeKm, 0, 'f', 1)
                           .arg(s.cruiseSpeedMs, 0, 'f', 2)
                           .arg(s.cruiseCurrentA, 0, 'f', 1)
                           .arg(s.enduranceMin, 0, 'f', 0);
        text += tr("\nSteepest climbable slope:");
        for (const physics::SlopeLimit& l : s.slopes) {
            text += tr("\n  %1: %2°%3")
                        .arg(fromView(physics::surfaceMaterialName(l.material)))
                        .arg(l.maxSlopeDeg, 0, 'f', 0)
                        .arg(l.tractionLimited ? tr(" (wheels slip)") : tr(" (motors stall)"));
        }
        figures_->setText(text);
        return;
    }
    const physics::AerialProfile* a = simulation_->aerialProfile();
    if (a == nullptr) {
        figures_->clear();
        return;
    }
    const physics::AerialProfile& p = *a;
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
    const QString key = simulation_->profileKey();
    const bool modified = simulation_->profileModified();
    // The vehicle is locked during a mission (FR-MIS-09).
    type_->setEnabled(!simulation_->missionActive());
    apply_->setEnabled(edited_);
    revert_->setEnabled(edited_ || modified);
    delete_->setEnabled(key.startsWith(QLatin1String("user.")) && !modified);
    if (edited_) {
        status_->setText(tr("Edited: Apply to use it."));
    } else if (modified) {
        status_->setText(tr("Using edited parameters (not saved)."));
    } else {
        status_->clear();
    }
}

void VehiclePanel::apply() {
    common::Result<void> applied;
    if (const physics::AerialProfile* a = simulation_->aerialProfile()) {
        physics::AerialProfile p = *a;
        const auto descriptors = physics::aerialParameters();
        for (std::size_t i = 0; i < params_.size(); ++i) {
            descriptors[i].set(p, params_[i]->value());
        }
        p.modelFile = modelFile_;
        applied = simulation_->applyProfile(p);
    } else if (const physics::GroundProfile* g = simulation_->groundProfile()) {
        physics::GroundProfile p = *g;
        const auto descriptors = physics::groundParameters();
        for (std::size_t i = 0; i < groundParams_.size(); ++i) {
            descriptors[i].set(p, groundParams_[i]->value());
        }
        p.modelFile = modelFile_;
        applied = simulation_->applyProfile(p);
    }
    if (!applied) {
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
        tr("%1 (copy)").arg(simulation_->profileName()), &ok);
    if (!ok) {
        return;
    }
    if (auto saved = simulation_->saveProfileAs(name); !saved) {
        showError(QString::fromStdString(saved.error().message));
    }
}

void VehiclePanel::remove() {
    const QString key = simulation_->profileKey();
    if (QMessageBox::question(this, tr("Delete Vehicle Profile"),
                              tr("Delete the profile “%1”?").arg(simulation_->profileName())) !=
        QMessageBox::Yes) {
        return;
    }
    if (auto removed = simulation_->deleteProfile(key); !removed) {
        showError(QString::fromStdString(removed.error().message));
    }
}

void VehiclePanel::chooseModel() {
    const QString file = QFileDialog::getOpenFileName(this, tr("Vehicle Model"), QString(),
                                                      tr("3D models (*.gltf *.glb *.obj)"));
    if (!file.isEmpty()) {
        setModelFile(file.toStdString(), true);
    }
}

void VehiclePanel::setModelFile(const std::string& file, bool edited) {
    modelFile_ = file;
    const QString name = QString::fromStdString(file);
    model_->setText(file.empty() ? tr("built-in") : QFileInfo(name).fileName());
    model_->setToolTip(name);
    builtInModel_->setEnabled(!file.empty());
    if (edited) {
        edited_ = true;
        updateButtons();
    }
}

void VehiclePanel::showError(const QString& message) {
    QMessageBox::warning(this, tr("Vehicle Profile"), message);
}

}  // namespace ds::app
