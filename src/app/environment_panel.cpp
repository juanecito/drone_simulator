// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/environment_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <cmath>

#include "ds/app/scene_controller.hpp"
#include "ds/astro/events.hpp"

namespace ds::app {
namespace {

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using LocalMs = std::chrono::local_time<milliseconds>;

constexpr std::array<double, 6> kRates{1.0, 10.0, 60.0, 600.0, 3600.0, 0.0};

// Wall-clock time in a zone as a QDateTime with no zone (only its fields are used).
QDateTime toWall(LocalMs local) {
    const qint64 ms = local.time_since_epoch().count();
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

LocalMs fromWall(const QDateTime& wall) {
    QDateTime utcFields(wall.date(), wall.time(), QTimeZone::UTC);
    return LocalMs(milliseconds(utcFields.toMSecsSinceEpoch()));
}

QString degrees(double value) { return QString::number(value, 'f', 1) + QStringLiteral("°"); }

}  // namespace

EnvironmentPanel::EnvironmentPanel(SceneController& scene, QWidget* parent)
    : QWidget(parent), scene_(&scene) {
    buildUi();
    refresh();
    connect(scene_, &SceneController::environmentChanged, this, &EnvironmentPanel::refresh);
    connect(scene_, &SceneController::terrainChanged, this, &EnvironmentPanel::refresh);
    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, [this] {
        if (scene_->environment().animate) {
            refresh();
        } else {
            updateReadouts();  // The camera may have moved.
        }
    });
    timer_->start();
}

void EnvironmentPanel::buildUi() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    auto* timeBox = new QGroupBox(tr("Date and time"));
    auto* form = new QFormLayout(timeBox);
    timeZone_ = new QComboBox;
    timeZone_->setObjectName(QStringLiteral("TimeZoneCombo"));
    timeZone_->setEditable(true);
    timeZone_->setInsertPolicy(QComboBox::NoInsert);
    timeZone_->addItem(QStringLiteral("UTC"));
    const QString system = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    if (!system.isEmpty() && system != QLatin1String("UTC")) {
        timeZone_->addItem(system);
    }
    for (const QByteArray& id : QTimeZone::availableTimeZoneIds()) {
        if (id.contains('/')) {
            timeZone_->addItem(QString::fromUtf8(id));
        }
    }
    form->addRow(tr("Time zone:"), timeZone_);
    connect(timeZone_, &QComboBox::activated, this, &EnvironmentPanel::onTimeZoneChosen);
    connect(timeZone_->lineEdit(), &QLineEdit::editingFinished, this,
            &EnvironmentPanel::onTimeZoneChosen);

    dateTime_ = new QDateTimeEdit;
    dateTime_->setObjectName(QStringLiteral("DateTimeEdit"));
    dateTime_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    dateTime_->setTimeZone(QTimeZone::UTC);
    dateTime_->setCalendarPopup(true);
    dateTime_->setKeyboardTracking(false);
    dateTime_->setDateTimeRange(
        QDateTime(QDate(astro::kMinSupportedYear, 1, 2), QTime(0, 0), QTimeZone::UTC),
        QDateTime(QDate(astro::kMaxSupportedYear, 12, 30), QTime(23, 59), QTimeZone::UTC));
    auto* now = new QPushButton(tr("Now"));
    now->setObjectName(QStringLiteral("NowButton"));
    auto* row = new QHBoxLayout;
    row->addWidget(dateTime_, 1);
    row->addWidget(now);
    form->addRow(tr("Local time:"), row);
    connect(dateTime_, &QDateTimeEdit::dateTimeChanged, this, &EnvironmentPanel::onDateTimeEdited);
    connect(now, &QPushButton::clicked, this, [this] {
        EnvironmentConfig c = scene_->environment();
        c.time = std::chrono::time_point_cast<milliseconds>(std::chrono::system_clock::now());
        apply(c);
    });

    slider_ = new QSlider(Qt::Horizontal);
    slider_->setObjectName(QStringLiteral("TimeSlider"));
    slider_->setRange(0, 24 * 60 - 1);
    slider_->setPageStep(60);
    form->addRow(tr("Time of day:"), slider_);
    connect(slider_, &QSlider::valueChanged, this, &EnvironmentPanel::onSliderMoved);

    animate_ = new QCheckBox(tr("Let time run"));
    animate_->setObjectName(QStringLiteral("AnimateCheck"));
    rate_ = new QComboBox;
    rate_->setObjectName(QStringLiteral("RateCombo"));
    rate_->addItems({tr("Real time"), QStringLiteral("× 10"), QStringLiteral("× 60"),
                     QStringLiteral("× 600"), QStringLiteral("× 3600")});
    auto* animRow = new QHBoxLayout;
    animRow->addWidget(animate_);
    animRow->addWidget(rate_, 1);
    form->addRow(QString(), animRow);
    connect(animate_, &QCheckBox::toggled, this, [this](bool on) {
        EnvironmentConfig c = scene_->environment();
        c.animate = on;
        apply(c);
    });
    connect(rate_, &QComboBox::activated, this, [this](int index) {
        EnvironmentConfig c = scene_->environment();
        c.timeRate = kRates[static_cast<std::size_t>(std::clamp(index, 0, 4))];
        apply(c);
    });
    layout->addWidget(timeBox);

    auto* skyBox = new QGroupBox(tr("Sun and Moon"));
    auto* skyForm = new QFormLayout(skyBox);
    sun_ = new QLabel;
    sun_->setObjectName(QStringLiteral("SunLabel"));
    moon_ = new QLabel;
    moon_->setObjectName(QStringLiteral("MoonLabel"));
    moon_->setWordWrap(true);
    events_ = new QLabel;
    events_->setObjectName(QStringLiteral("EventsLabel"));
    events_->setTextFormat(Qt::RichText);
    skyForm->addRow(tr("Sun:"), sun_);
    skyForm->addRow(tr("Moon:"), moon_);
    skyForm->addRow(events_);
    layout->addWidget(skyBox);

    auto* camBox = new QGroupBox(tr("Exposure and visibility"));
    auto* camForm = new QFormLayout(camBox);
    exposureMode_ = new QComboBox;
    exposureMode_->setObjectName(QStringLiteral("ExposureModeCombo"));
    exposureMode_->addItems({tr("Automatic"), tr("Manual")});
    camForm->addRow(tr("Exposure:"), exposureMode_);
    manualEv_ = new QDoubleSpinBox;
    manualEv_->setObjectName(QStringLiteral("ManualEvSpin"));
    manualEv_->setRange(graphics::kMinEv100, graphics::kMaxEv100);
    manualEv_->setSingleStep(0.5);
    manualEv_->setDecimals(1);
    manualEv_->setPrefix(QStringLiteral("EV "));
    manualEv_->setKeyboardTracking(false);
    camForm->addRow(tr("Manual EV₁₀₀:"), manualEv_);
    compensation_ = new QDoubleSpinBox;
    compensation_->setObjectName(QStringLiteral("CompensationSpin"));
    compensation_->setRange(-6.0, 6.0);
    compensation_->setSingleStep(0.5);
    compensation_->setDecimals(1);
    compensation_->setSuffix(tr(" EV"));
    compensation_->setToolTip(tr("Positive values make the automatic exposure darker."));
    compensation_->setKeyboardTracking(false);
    camForm->addRow(tr("Compensation:"), compensation_);
    measured_ = new QLabel(QStringLiteral("—"));
    measured_->setObjectName(QStringLiteral("MeasuredEvLabel"));
    camForm->addRow(tr("Measured:"), measured_);
    visibility_ = new QDoubleSpinBox;
    visibility_->setObjectName(QStringLiteral("VisibilitySpin"));
    visibility_->setRange(kMinVisibilityM / 1000.0, kMaxVisibilityM / 1000.0);
    visibility_->setDecimals(2);
    visibility_->setSuffix(tr(" km"));
    visibility_->setKeyboardTracking(false);
    visibility_->setToolTip(tr("Meteorological visibility: haze and fog."));
    camForm->addRow(tr("Visibility:"), visibility_);
    const auto exposureChanged = [this] {
        EnvironmentConfig c = scene_->environment();
        c.exposure.mode = exposureMode_->currentIndex() == 1 ? graphics::ExposureMode::Manual
                                                             : graphics::ExposureMode::Auto;
        c.exposure.manualEv100 = manualEv_->value();
        c.exposure.compensationEv = compensation_->value();
        c.visibilityM = visibility_->value() * 1000.0;
        apply(c);
    };
    connect(exposureMode_, &QComboBox::activated, this, exposureChanged);
    connect(manualEv_, &QDoubleSpinBox::valueChanged, this, exposureChanged);
    connect(compensation_, &QDoubleSpinBox::valueChanged, this, exposureChanged);
    connect(visibility_, &QDoubleSpinBox::valueChanged, this, exposureChanged);
    layout->addWidget(camBox);

    // Wind and air (FR-WIND-01, FR-PHY-05).
    auto* airBox = new QGroupBox(tr("Wind and air"));
    auto* airForm = new QFormLayout(airBox);
    windLevel_ = new QSpinBox;
    windLevel_->setObjectName(QStringLiteral("WindLevelSpin"));
    windLevel_->setRange(0, weather::WindLevelTable::kLevelCount - 1);
    windLevel_->setKeyboardTracking(false);
    airForm->addRow(tr("Wind level:"), windLevel_);
    windInfo_ = new QLabel;
    windInfo_->setObjectName(QStringLiteral("WindInfoLabel"));
    windInfo_->setWordWrap(true);
    airForm->addRow(QString(), windInfo_);
    windFrom_ = new QSpinBox;
    windFrom_->setObjectName(QStringLiteral("WindFromSpin"));
    windFrom_->setRange(0, 359);
    windFrom_->setWrapping(true);
    windFrom_->setSuffix(QStringLiteral("°"));
    windFrom_->setKeyboardTracking(false);
    windFrom_->setToolTip(tr("Direction the wind blows from (true north = 0°, east = 90°)."));
    windRandom_ = new QCheckBox(tr("Random"));
    windRandom_->setObjectName(QStringLiteral("WindRandomCheck"));
    auto* fromRow = new QHBoxLayout;
    fromRow->addWidget(windFrom_, 1);
    fromRow->addWidget(windRandom_);
    airForm->addRow(tr("Wind from:"), fromRow);
    temperature_ = new QDoubleSpinBox;
    temperature_->setObjectName(QStringLiteral("TemperatureSpin"));
    temperature_->setRange(kMinTemperatureC, kMaxTemperatureC);
    temperature_->setDecimals(1);
    temperature_->setSuffix(QStringLiteral(" °C"));
    temperature_->setKeyboardTracking(false);
    temperature_->setToolTip(tr("Air temperature at the ground (air density, battery)."));
    airForm->addRow(tr("Temperature:"), temperature_);
    qnh_ = new QDoubleSpinBox;
    qnh_->setObjectName(QStringLiteral("QnhSpin"));
    qnh_->setRange(kMinQnhHpa, kMaxQnhHpa);
    qnh_->setDecimals(1);
    qnh_->setSuffix(QStringLiteral(" hPa"));
    qnh_->setKeyboardTracking(false);
    qnh_->setToolTip(tr("Pressure reduced to mean sea level (altimeter setting)."));
    airForm->addRow(tr("QNH:"), qnh_);
    humidity_ = new QDoubleSpinBox;
    humidity_->setObjectName(QStringLiteral("HumiditySpin"));
    humidity_->setRange(0.0, 100.0);
    humidity_->setDecimals(0);
    humidity_->setSuffix(QStringLiteral(" %"));
    humidity_->setKeyboardTracking(false);
    airForm->addRow(tr("Humidity:"), humidity_);
    const auto airChanged = [this] {
        EnvironmentConfig c = scene_->environment();
        c.wind.level = windLevel_->value();
        c.wind.fromDeg = windFrom_->value();
        c.wind.randomDirection = windRandom_->isChecked();
        c.air.temperatureC = temperature_->value();
        c.air.pressureHpa = qnh_->value();
        c.air.humidityPct = humidity_->value();
        apply(c);
        updateWindInfo();
        updateEnabled();
    };
    connect(windLevel_, &QSpinBox::valueChanged, this, airChanged);
    connect(windFrom_, &QSpinBox::valueChanged, this, airChanged);
    connect(windRandom_, &QCheckBox::toggled, this, airChanged);
    connect(temperature_, &QDoubleSpinBox::valueChanged, this, airChanged);
    connect(qnh_, &QDoubleSpinBox::valueChanged, this, airChanged);
    connect(humidity_, &QDoubleSpinBox::valueChanged, this, airChanged);
    layout->addWidget(airBox);

    auto* note = new QLabel(tr("The weather generator arrives in milestone M7."));
    note->setEnabled(false);
    layout->addWidget(note);
    layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

void EnvironmentPanel::apply(const EnvironmentConfig& config) { scene_->setEnvironment(config); }

void EnvironmentPanel::refresh() {
    const EnvironmentConfig& env = scene_->environment();
    const auto local = astro::toLocalTime(env.time, env.timeZone);
    {
        const QSignalBlocker b1(timeZone_);
        const QSignalBlocker b2(dateTime_);
        const QSignalBlocker b3(slider_);
        const QSignalBlocker b4(animate_);
        const QSignalBlocker b5(rate_);
        const QSignalBlocker b6(exposureMode_);
        const QSignalBlocker b7(manualEv_);
        const QSignalBlocker b8(compensation_);
        const QSignalBlocker b9(visibility_);
        const QSignalBlocker b10(windLevel_);
        const QSignalBlocker b11(windFrom_);
        const QSignalBlocker b12(windRandom_);
        const QSignalBlocker b13(temperature_);
        const QSignalBlocker b14(qnh_);
        const QSignalBlocker b15(humidity_);
        timeZone_->setCurrentText(QString::fromStdString(env.timeZone));
        if (local) {
            const QDateTime wall = toWall(local->time);
            dateTime_->setDateTime(wall);
            slider_->setValue(wall.time().hour() * 60 + wall.time().minute());
            dateTime_->setToolTip(
                QStringLiteral("%1 (UTC%2%3)")
                    .arg(QString::fromStdString(local->abbreviation))
                    .arg(local->utcOffset.count() >= 0 ? QStringLiteral("+") : QString())
                    .arg(QString::number(static_cast<double>(local->utcOffset.count()) / 3600.0)));
        }
        animate_->setChecked(env.animate);
        int rateIndex = 0;
        for (std::size_t i = 0; i < 5; ++i) {
            if (std::abs(kRates[i] - env.timeRate) < 1e-9) {
                rateIndex = static_cast<int>(i);
            }
        }
        rate_->setCurrentIndex(rateIndex);
        exposureMode_->setCurrentIndex(env.exposure.mode == graphics::ExposureMode::Manual ? 1 : 0);
        manualEv_->setValue(env.exposure.manualEv100);
        compensation_->setValue(env.exposure.compensationEv);
        visibility_->setValue(env.visibilityM / 1000.0);
        windLevel_->setValue(env.wind.level);
        windFrom_->setValue(static_cast<int>(std::lround(env.wind.fromDeg)) % 360);
        windRandom_->setChecked(env.wind.randomDirection);
        temperature_->setValue(env.air.temperatureC);
        qnh_->setValue(env.air.pressureHpa);
        humidity_->setValue(env.air.humidityPct);
    }
    updateWindInfo();
    updateEnabled();
    updateReadouts();
    updateEvents();
}

void EnvironmentPanel::updateEnabled() {
    const bool manual = exposureMode_->currentIndex() == 1;
    manualEv_->setEnabled(manual);
    compensation_->setEnabled(!manual);
    rate_->setEnabled(animate_->isChecked());
    windFrom_->setEnabled(!windRandom_->isChecked());
}

void EnvironmentPanel::updateWindInfo() {
    const weather::WindLevel& l = weather::WindLevelTable::builtIn().level(windLevel_->value());
    if (l.level == 0) {
        windInfo_->setText(tr("%1: no wind.").arg(QString::fromStdString(l.name)));
        return;
    }
    windInfo_->setText(tr("%1: %2 m/s mean at 10 m, gusts to %3 m/s, %4 turbulence.")
                           .arg(QString::fromStdString(l.name))
                           .arg(l.meanSpeedMs, 0, 'f', 1)
                           .arg(l.gustPeakMs, 0, 'f', 1)
                           .arg(QString::fromStdString(l.turbulence)));
}

void EnvironmentPanel::onDateTimeEdited() {
    EnvironmentConfig c = scene_->environment();
    auto utc = astro::fromLocalTime(fromWall(dateTime_->dateTime()), c.timeZone);
    if (!utc || !astro::isSupported(*utc)) {
        refresh();
        return;
    }
    c.time = *utc;
    apply(c);
}

void EnvironmentPanel::onSliderMoved(int minute) {
    QDateTime wall = dateTime_->dateTime();
    wall.setTime(QTime(minute / 60, minute % 60));
    EnvironmentConfig c = scene_->environment();
    if (auto utc = astro::fromLocalTime(fromWall(wall), c.timeZone)) {
        c.time = *utc;
        apply(c);
    }
}

void EnvironmentPanel::onTimeZoneChosen() {
    const std::string zone = timeZone_->currentText().trimmed().toStdString();
    EnvironmentConfig c = scene_->environment();
    if (zone == c.timeZone) {
        return;
    }
    if (!astro::toLocalTime(c.time, zone)) {
        const QSignalBlocker blocker(timeZone_);
        timeZone_->setCurrentText(QString::fromStdString(c.timeZone));
        return;
    }
    c.timeZone = zone;
    apply(c);
}

QString EnvironmentPanel::phaseName(double e) {
    e = std::fmod(std::fmod(e, 360.0) + 360.0, 360.0);
    if (e < 6.0 || e >= 354.0) {
        return tr("New moon");
    }
    if (e < 84.0) {
        return tr("Waxing crescent");
    }
    if (e < 96.0) {
        return tr("First quarter");
    }
    if (e < 174.0) {
        return tr("Waxing gibbous");
    }
    if (e < 186.0) {
        return tr("Full moon");
    }
    if (e < 264.0) {
        return tr("Waning gibbous");
    }
    if (e < 276.0) {
        return tr("Last quarter");
    }
    return tr("Waning crescent");
}

QString EnvironmentPanel::formatLocal(astro::UtcTime time, bool withDate) const {
    const auto local = astro::toLocalTime(time, scene_->environment().timeZone);
    if (!local) {
        return QStringLiteral("—");
    }
    const QDateTime wall = toWall(local->time);
    return wall.toString(withDate ? QStringLiteral("yyyy-MM-dd HH:mm") : QStringLiteral("HH:mm"));
}

void EnvironmentPanel::updateReadouts() {
    const graphics::SkyState& s = scene_->sky().state();
    sun_->setText(
        tr("azimuth %1, elevation %2").arg(degrees(s.sun.azimuthDeg), degrees(s.sun.elevationDeg)));
    moon_->setText(tr("azimuth %1, elevation %2\n%3, %4 % illuminated")
                       .arg(degrees(s.moon.azimuthDeg), degrees(s.moon.elevationDeg),
                            phaseName(s.moonPhase.elongationDeg))
                       .arg(std::lround(s.moonPhase.illuminatedFraction * 100.0)));
}

void EnvironmentPanel::updateEvents() {
    const EnvironmentConfig& env = scene_->environment();
    const auto local = astro::toLocalTime(env.time, env.timeZone);
    if (!local) {
        return;
    }
    const QDate date = toWall(local->time).date();
    const geo::Geodetic observer = scene_->terrain().origin;
    if (date == eventsDate_ && env.timeZone == eventsZone_ && observer.latitudeDeg == eventsLat_) {
        return;
    }
    eventsDate_ = date;
    eventsZone_ = env.timeZone;
    eventsLat_ = observer.latitudeDeg;
    const QDateTime midnight(date, QTime(0, 0), QTimeZone::UTC);
    auto start = astro::fromLocalTime(fromWall(midnight), env.timeZone);
    if (!start) {
        return;
    }
    const astro::SunEvents sun = astro::sunEvents(*start, observer);
    const astro::MoonEvents moon = astro::moonEvents(*start, observer);
    const auto t = [this](const std::optional<astro::UtcTime>& time) {
        return time ? formatLocal(*time) : QStringLiteral("—");
    };
    QString text = QStringLiteral("<table cellspacing=2>");
    const auto row = [&text](const QString& name, const QString& value) {
        text += QStringLiteral("<tr><td>%1</td><td align=right>%2</td></tr>").arg(name, value);
    };
    if (sun.alwaysUp) {
        row(tr("Sun"), tr("up all day (polar day)"));
    } else if (sun.alwaysDown) {
        row(tr("Sun"), tr("down all day (polar night)"));
    }
    row(tr("Astronomical dawn"), t(sun.astronomicalDawn));
    row(tr("Nautical dawn"), t(sun.nauticalDawn));
    row(tr("Civil dawn"), t(sun.civilDawn));
    row(tr("Sunrise"), t(sun.sunrise));
    row(tr("Solar noon"), t(sun.solarNoon));
    row(tr("Sunset"), t(sun.sunset));
    row(tr("Civil dusk"), t(sun.civilDusk));
    row(tr("Nautical dusk"), t(sun.nauticalDusk));
    row(tr("Astronomical dusk"), t(sun.astronomicalDusk));
    row(tr("Moonrise"), t(moon.moonrise));
    row(tr("Moonset"), t(moon.moonset));
    row(tr("Next full moon"),
        formatLocal(astro::nextLunarPhase(*start, astro::LunarPhase::FullMoon), true));
    row(tr("Next new moon"),
        formatLocal(astro::nextLunarPhase(*start, astro::LunarPhase::NewMoon), true));
    text += QStringLiteral("</table>");
    events_->setText(text);
}

void EnvironmentPanel::setMeasuredExposure(double ev100, double averageLuminance) {
    measured_->setText(
        tr("EV %1 (average %2 cd/m²)")
            .arg(QString::number(ev100, 'f', 1), QString::number(averageLuminance, 'g', 3)));
}

}  // namespace ds::app
