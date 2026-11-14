// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/controls_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

#include "ds/app/settings_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/common/log.hpp"
#include "ds/input/joystick_service.hpp"
#include "ds/storage/input_profile_store.hpp"
#include "ds/storage/storage_service.hpp"

namespace ds::app {
namespace {

constexpr const char* kVehicleType = "aerial";
constexpr int kPollMs = 30;

// Mapping combo entries: saved profiles carry their row id, presets a negative code.
enum PresetCode : int {
    kRadioAetr = -1,
    kRadioTaer = -2,
    kGamepadMode2 = -3,
    kGamepadMode1 = -4,
};

QString fromStd(const std::string& text) { return QString::fromStdString(text); }

int percent(double v) { return static_cast<int>(std::lround(100.0 * v)); }

}  // namespace

// --- Calibration dialog ---------------------------------------------------------------------

CalibrationDialog::CalibrationDialog(const input::JoystickService& joysticks, std::string guid,
                                     int axes, input::InputMapping base, QWidget* parent)
    : QDialog(parent),
      joysticks_(&joysticks),
      guid_(std::move(guid)),
      wizard_(axes, std::move(base)) {
    setWindowTitle(tr("Calibrate Controller"));
    setObjectName(QStringLiteral("CalibrationDialog"));
    auto* layout = new QVBoxLayout(this);
    prompt_ = new QLabel;
    prompt_->setObjectName(QStringLiteral("CalibrationPrompt"));
    prompt_->setWordWrap(true);
    QFont big = prompt_->font();
    big.setPointSizeF(big.pointSizeF() * 1.2);
    big.setBold(true);
    prompt_->setFont(big);
    layout->addWidget(prompt_);

    auto* grid = new QGridLayout;
    for (int i = 0; i < axes; ++i) {
        auto* bar = new QProgressBar;
        bar->setRange(-100, 100);
        bar->setFormat(QStringLiteral("%v"));
        bar->setMaximumHeight(16);
        grid->addWidget(new QLabel(tr("Axis %1").arg(i)), i, 0);
        grid->addWidget(bar, i, 1);
        bars_.push_back(bar);
    }
    layout->addLayout(grid);
    error_ = new QLabel;
    error_->setObjectName(QStringLiteral("CalibrationError"));
    error_->setStyleSheet(QStringLiteral("color: #c03030;"));
    error_->setWordWrap(true);
    layout->addWidget(error_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    next_ = buttons->addButton(tr("Next"), QDialogButtonBox::ActionRole);
    next_->setDefault(true);
    connect(next_, &QPushButton::clicked, this, &CalibrationDialog::next);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &CalibrationDialog::poll);
    timer_->start(20);
    updatePrompt();
}

void CalibrationDialog::poll() {
    const input::RawJoystickState raw = joysticks_->state(guid_);
    if (!raw.connected) {
        error_->setText(tr("The controller is not connected."));
        return;
    }
    wizard_.sample(raw);
    const int active = wizard_.mostDeflectedAxis();
    for (std::size_t i = 0; i < bars_.size() && i < raw.axes.size(); ++i) {
        bars_[i]->setValue(percent(raw.axes[i]));
        bars_[i]->setStyleSheet(static_cast<int>(i) == active
                                    ? QStringLiteral("QProgressBar::chunk { background: #2e8b57; }")
                                    : QString());
    }
}

void CalibrationDialog::next() {
    if (!wizard_.next()) {
        const auto e = wizard_.error();
        error_->setText(QString::fromUtf8(e.data(), static_cast<qsizetype>(e.size())));
        return;
    }
    error_->clear();
    if (wizard_.step() == input::CalibrationWizard::Step::Done) {
        accept();
        return;
    }
    updatePrompt();
}

void CalibrationDialog::updatePrompt() {
    const auto p = wizard_.prompt();
    prompt_->setText(QString::fromUtf8(p.data(), static_cast<qsizetype>(p.size())));
}

// --- Controls panel -------------------------------------------------------------------------

ControlsPanel::ControlsPanel(SimulationController& simulation, SettingsController* settings,
                             QWidget* parent)
    : QWidget(parent), simulation_(&simulation), settings_(settings) {
    if (simulation_->joystickMapping()) {
        working_ = *simulation_->joystickMapping();
    }
    buildUi();
    refreshDevices();
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &ControlsPanel::poll);
    timer_->start(kPollMs);
}

void ControlsPanel::buildUi() {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* box = new QGroupBox(tr("Controls"));
    outer->addWidget(box);
    auto* layout = new QVBoxLayout(box);

    auto* form = new QFormLayout;
    device_ = new QComboBox;
    device_->setObjectName(QStringLiteral("DeviceCombo"));
    connect(device_, &QComboBox::activated, this, &ControlsPanel::onDeviceChosen);
    form->addRow(tr("Device:"), device_);
    mapping_ = new QComboBox;
    mapping_->setObjectName(QStringLiteral("MappingCombo"));
    connect(mapping_, &QComboBox::activated, this, &ControlsPanel::onMappingChosen);
    form->addRow(tr("Mapping:"), mapping_);
    layout->addLayout(form);

    auto* buttons = new QHBoxLayout;
    use_ = new QPushButton(tr("Fly With This"));
    use_->setObjectName(QStringLiteral("UseDeviceButton"));
    calibrate_ = new QPushButton(tr("Calibrate…"));
    calibrate_->setObjectName(QStringLiteral("CalibrateButton"));
    save_ = new QPushButton(tr("Save…"));
    save_->setObjectName(QStringLiteral("SaveMappingButton"));
    delete_ = new QPushButton(tr("Delete"));
    delete_->setObjectName(QStringLiteral("DeleteMappingButton"));
    for (QPushButton* b : {use_, calibrate_, save_, delete_}) {
        buttons->addWidget(b);
    }
    layout->addLayout(buttons);
    connect(use_, &QPushButton::clicked, this, &ControlsPanel::use);
    connect(calibrate_, &QPushButton::clicked, this, &ControlsPanel::calibrate);
    connect(save_, &QPushButton::clicked, this, &ControlsPanel::save);
    connect(delete_, &QPushButton::clicked, this, &ControlsPanel::remove);

    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("<b>Channel</b>")), 0, 0);
    grid->addWidget(new QLabel(tr("<b>Axis</b>")), 0, 1);
    grid->addWidget(new QLabel(tr("<b>Reverse</b>")), 0, 2);
    grid->addWidget(new QLabel(tr("<b>Expo</b>")), 0, 3);
    grid->addWidget(new QLabel(tr("<b>Deadzone</b>")), 0, 4);
    grid->addWidget(new QLabel(tr("<b>Live</b>")), 0, 5);
    const std::array<QString, input::kChannelCount> names{tr("Throttle"), tr("Roll"), tr("Pitch"),
                                                          tr("Yaw")};
    for (std::size_t i = 0; i < input::kChannelCount; ++i) {
        ChannelRow& r = rows_[i];
        const int row = static_cast<int>(i) + 1;
        r.axis = new QLabel;
        r.inverted = new QCheckBox;
        r.expo = new QDoubleSpinBox;
        r.expo->setRange(0.0, 1.0);
        r.expo->setSingleStep(0.05);
        r.expo->setDecimals(2);
        r.deadzone = new QDoubleSpinBox;
        r.deadzone->setRange(0.0, 0.25);
        r.deadzone->setSingleStep(0.01);
        r.deadzone->setDecimals(2);
        r.bar = new QProgressBar;
        r.bar->setRange(i == 0 ? 0 : -100, 100);
        r.bar->setFormat(QStringLiteral("%v"));
        r.bar->setMaximumHeight(16);
        r.bar->setObjectName(QStringLiteral("LiveBar%1").arg(row));
        grid->addWidget(new QLabel(names[i]), row, 0);
        grid->addWidget(r.axis, row, 1);
        grid->addWidget(r.inverted, row, 2);
        grid->addWidget(r.expo, row, 3);
        grid->addWidget(r.deadzone, row, 4);
        grid->addWidget(r.bar, row, 5);
        connect(r.inverted, &QCheckBox::toggled, this, &ControlsPanel::readChannels);
        connect(r.expo, &QDoubleSpinBox::valueChanged, this, &ControlsPanel::readChannels);
        connect(r.deadzone, &QDoubleSpinBox::valueChanged, this, &ControlsPanel::readChannels);
    }
    grid->setColumnStretch(5, 1);
    layout->addLayout(grid);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("InputStatusLabel"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    if (simulation_->joysticks() == nullptr) {
        auto* note =
            new QLabel(tr("Joysticks are not available (SDL could not be started); "
                          "fly with the keyboard."));
        note->setWordWrap(true);
        note->setEnabled(false);
        layout->addWidget(note);
    }
}

std::optional<std::string> ControlsPanel::selectedGuid() const {
    const QString guid = device_->currentData().toString();
    if (guid.isEmpty()) {
        return std::nullopt;
    }
    return guid.toStdString();
}

void ControlsPanel::refreshDevices() {
    input::JoystickService* joysticks = simulation_->joysticks();
    const std::uint64_t version = joysticks != nullptr ? joysticks->devicesVersion() : 0;
    if (version == devicesVersion_) {
        return;
    }
    devicesVersion_ = version;
    // Keep the selection: the device in use, else the one shown.
    std::string keep;
    if (const auto guid = selectedGuid()) {
        keep = *guid;
    } else if (simulation_->joystickMapping() && device_->count() == 0) {
        keep = simulation_->joystickMapping()->deviceGuid;
    }
    {
        const QSignalBlocker block(device_);
        device_->clear();
        device_->addItem(tr("Keyboard"), QString());
        if (joysticks != nullptr) {
            for (const input::DeviceInfo& d : joysticks->devices()) {
                device_->addItem(QStringLiteral("%1 (%2 axes%3)")
                                     .arg(fromStd(d.name))
                                     .arg(d.axes)
                                     .arg(d.gamepad ? tr(", gamepad") : QString()),
                                 fromStd(d.guid));
            }
        }
        // A remembered device that is unplugged stays selectable.
        if (!keep.empty() && device_->findData(fromStd(keep)) < 0 &&
            simulation_->joystickMapping() && simulation_->joystickMapping()->deviceGuid == keep) {
            device_->addItem(
                tr("%1 (not connected)").arg(fromStd(simulation_->joystickMapping()->deviceName)),
                fromStd(keep));
        }
        const int index = keep.empty() ? 0 : device_->findData(fromStd(keep));
        device_->setCurrentIndex(std::max(index, 0));
    }
    reloadMappings();
    updateStatus();
}

void ControlsPanel::onDeviceChosen() {
    reloadMappings();
    onMappingChosen();
}

void ControlsPanel::reloadMappings() {
    const QSignalBlocker block(mapping_);
    mapping_->clear();
    const auto guid = selectedGuid();
    const bool joystick = guid.has_value();
    for (QWidget* w : std::initializer_list<QWidget*>{mapping_, calibrate_, save_, delete_}) {
        w->setEnabled(joystick);
    }
    for (ChannelRow& r : rows_) {
        r.inverted->setEnabled(joystick);
        r.expo->setEnabled(joystick);
        r.deadzone->setEnabled(joystick);
    }
    if (!joystick) {
        showMapping();
        return;
    }
    if (settings_ != nullptr) {
        auto records = settings_->storage()
                           .submit([g = *guid](storage::DbConnection& c) {
                               storage::InputProfileStore store(c);
                               return store.list(g);
                           })
                           .get();
        if (records) {
            for (const auto& r : *records) {
                if (r.vehicleType == kVehicleType && r.id) {
                    mapping_->addItem(fromStd(r.name), QVariant::fromValue(*r.id));
                }
            }
        } else {
            log::warn("Cannot list input profiles: {}", records.error().message);
        }
    }
    mapping_->addItem(tr("Preset: radio, AETR"), static_cast<int>(kRadioAetr));
    mapping_->addItem(tr("Preset: radio, TAER"), static_cast<int>(kRadioTaer));
    mapping_->addItem(tr("Preset: gamepad, mode 2"), static_cast<int>(kGamepadMode2));
    mapping_->addItem(tr("Preset: gamepad, mode 1"), static_cast<int>(kGamepadMode1));

    // Show the mapping in use for this device, else its first saved profile or a preset.
    const auto& active = simulation_->joystickMapping();
    if (active && active->deviceGuid == *guid) {
        working_ = *active;
        const int saved = mapping_->findText(fromStd(active->name));
        mapping_->setCurrentIndex(std::max(saved, 0));
        showMapping();
        return;
    }
    bool gamepad = false;
    if (input::JoystickService* js = simulation_->joysticks()) {
        for (const input::DeviceInfo& d : js->devices()) {
            gamepad = gamepad || (d.guid == *guid && d.gamepad);
        }
    }
    if (mapping_->itemData(0).toLongLong() < 0) {
        mapping_->setCurrentIndex(
            mapping_->findData(static_cast<int>(gamepad ? kGamepadMode2 : kRadioAetr)));
    }
    onMappingChosen();
}

void ControlsPanel::onMappingChosen() {
    const auto guid = selectedGuid();
    if (!guid) {
        showMapping();
        return;
    }
    const qlonglong code = mapping_->currentData().toLongLong();
    common::Result<input::InputMapping> m = common::makeError("No mapping.");
    switch (code) {
        case kRadioAetr: m = input::radioMapping("AETR"); break;
        case kRadioTaer: m = input::radioMapping("TAER"); break;
        case kGamepadMode2: m = input::gamepadMapping(2); break;
        case kGamepadMode1: m = input::gamepadMapping(1); break;
        default: {
            if (settings_ == nullptr) {
                break;
            }
            const std::string name = mapping_->currentText().toStdString();
            auto record = settings_->storage()
                              .submit([g = *guid, name](storage::DbConnection& c) {
                                  storage::InputProfileStore store(c);
                                  return store.find(g, kVehicleType, name);
                              })
                              .get();
            if (record && *record) {
                if (auto json = common::parseJson((*record)->mappingJson)) {
                    m = input::InputMapping::fromJson(*json);
                }
            }
            break;
        }
    }
    if (!m) {
        status_->setText(tr("Cannot load the mapping: %1").arg(fromStd(m.error().message)));
        return;
    }
    working_ = *m;
    working_.deviceGuid = *guid;
    if (input::JoystickService* js = simulation_->joysticks()) {
        for (const input::DeviceInfo& d : js->devices()) {
            if (d.guid == *guid) {
                working_.deviceName = d.name;
            }
        }
    }
    if (code >= 0) {
        working_.name = mapping_->currentText().toStdString();
    }
    delete_->setEnabled(code >= 0);
    showMapping();
}

void ControlsPanel::showMapping() {
    const bool joystick = selectedGuid().has_value();
    for (std::size_t i = 0; i < input::kChannelCount; ++i) {
        ChannelRow& r = rows_[i];
        const input::AxisCalibration& c = working_.channels[i];
        const QSignalBlocker b1(r.inverted);
        const QSignalBlocker b2(r.expo);
        const QSignalBlocker b3(r.deadzone);
        r.axis->setText(!joystick     ? QStringLiteral("—")
                        : c.axis >= 0 ? tr("Axis %1").arg(c.axis)
                                      : tr("none"));
        r.inverted->setChecked(c.inverted);
        r.expo->setValue(c.expo);
        r.deadzone->setValue(c.deadzone);
    }
    updateStatus();
}

void ControlsPanel::readChannels() {
    for (std::size_t i = 0; i < input::kChannelCount; ++i) {
        input::AxisCalibration& c = working_.channels[i];
        c.inverted = rows_[i].inverted->isChecked();
        c.expo = rows_[i].expo->value();
        c.deadzone = rows_[i].deadzone->value();
    }
}

void ControlsPanel::poll() {
    if (!isVisible()) {
        return;
    }
    refreshDevices();
    const auto guid = selectedGuid();
    input::JoystickService* js = simulation_->joysticks();
    if (!guid || js == nullptr) {
        // Keyboard: show what the simulation receives.
        const input::ControlInputs& c = simulation_->snapshot().controls;
        const std::array<double, 4> v{c.throttle, c.roll, c.pitch, c.yaw};
        for (std::size_t i = 0; i < input::kChannelCount; ++i) {
            rows_[i].bar->setValue(percent(v[i]));
        }
        return;
    }
    const input::RawJoystickState raw = js->state(*guid);
    const input::ControlInputs c =
        input::mapControls(working_, raw, previousRaw_, previousControls_);
    previousRaw_ = raw;
    previousControls_ = c;
    const std::array<double, 4> v{c.throttle, c.roll, c.pitch, c.yaw};
    for (std::size_t i = 0; i < input::kChannelCount; ++i) {
        rows_[i].bar->setValue(percent(v[i]));
    }
}

void ControlsPanel::use() {
    if (const auto guid = selectedGuid()) {
        readChannels();
        simulation_->setJoystickMapping(working_);
    } else {
        simulation_->setJoystickMapping(std::nullopt);
    }
    updateStatus();
}

void ControlsPanel::calibrate() {
    const auto guid = selectedGuid();
    input::JoystickService* js = simulation_->joysticks();
    if (!guid || js == nullptr) {
        return;
    }
    int axes = 0;
    for (const input::DeviceInfo& d : js->devices()) {
        if (d.guid == *guid) {
            axes = d.axes;
        }
    }
    if (axes < 4) {
        QMessageBox::warning(this, tr("Calibrate Controller"),
                             tr("The controller must be connected and have at least 4 axes."));
        return;
    }
    readChannels();
    CalibrationDialog dialog(*js, *guid, axes, working_, this);
    if (dialog.exec() == QDialog::Accepted) {
        working_ = dialog.result();
        working_.deviceGuid = *guid;
        showMapping();
        status_->setText(tr("Calibrated. Save the mapping and press “Fly With This”."));
    }
}

void ControlsPanel::save() {
    const auto guid = selectedGuid();
    if (!guid || settings_ == nullptr) {
        return;
    }
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Save Mapping"), tr("Mapping name:"), QLineEdit::Normal,
                              fromStd(working_.name.empty() ? "Default" : working_.name), &ok)
            .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    readChannels();
    working_.name = name.toStdString();
    working_.deviceGuid = *guid;
    const storage::InputProfileRecord record{.deviceGuid = *guid,
                                             .vehicleType = kVehicleType,
                                             .name = working_.name,
                                             .mappingJson = working_.toJson().dump()};
    auto saved = settings_->storage()
                     .submit([record](storage::DbConnection& c) {
                         storage::InputProfileStore store(c);
                         return store.save(record);
                     })
                     .get();
    if (!saved) {
        QMessageBox::warning(this, tr("Save Mapping"), fromStd(saved.error().message));
        return;
    }
    const input::InputMapping keep = working_;
    reloadMappings();
    working_ = keep;
    mapping_->setCurrentIndex(std::max(mapping_->findText(name), 0));
    delete_->setEnabled(true);
    // A mapping being flown is updated at once.
    if (simulation_->joystickMapping() && simulation_->joystickMapping()->deviceGuid == *guid) {
        simulation_->setJoystickMapping(working_);
    }
    showMapping();
}

void ControlsPanel::remove() {
    const qlonglong id = mapping_->currentData().toLongLong();
    if (id < 0 || settings_ == nullptr) {
        return;
    }
    if (QMessageBox::question(this, tr("Delete Mapping"),
                              tr("Delete the mapping “%1”?").arg(mapping_->currentText())) !=
        QMessageBox::Yes) {
        return;
    }
    auto removed = settings_->storage()
                       .submit([id](storage::DbConnection& c) {
                           storage::InputProfileStore store(c);
                           return store.remove(id);
                       })
                       .get();
    if (!removed) {
        QMessageBox::warning(this, tr("Delete Mapping"), fromStd(removed.error().message));
        return;
    }
    reloadMappings();
    onMappingChosen();
}

void ControlsPanel::updateStatus() {
    const auto& active = simulation_->joystickMapping();
    if (!active) {
        status_->setText(tr("Flying with the keyboard."));
        return;
    }
    status_->setText(
        tr("Flying with %1, mapping “%2”.")
            .arg(fromStd(active->deviceName.empty() ? active->deviceGuid : active->deviceName),
                 fromStd(active->name)));
}

}  // namespace ds::app
