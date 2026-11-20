// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/environment_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

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

const weather::WeatherPresetTable& presets() { return weather::WeatherPresetTable::builtIn(); }

constexpr std::array<weather::CloudType, 5> kCloudTypes{
    weather::CloudType::Cumulus, weather::CloudType::Stratocumulus, weather::CloudType::Stratus,
    weather::CloudType::Cirrus, weather::CloudType::Cumulonimbus};
constexpr std::array<weather::PrecipitationType, 5> kPrecipitationTypes{
    weather::PrecipitationType::None, weather::PrecipitationType::Drizzle,
    weather::PrecipitationType::Rain, weather::PrecipitationType::Snow,
    weather::PrecipitationType::Sleet};
constexpr std::array<weather::Intensity, 3> kIntensities{
    weather::Intensity::Light, weather::Intensity::Moderate, weather::Intensity::Heavy};

// Timeline entry of a free-mode weather that has none yet.
weather::WeatherChange defaultChange() {
    weather::WeatherChange c;
    c.atS = 600.0;
    c.transitionS = 300.0;
    c.preset = "light-rain";
    return c;
}

int presetIndex(QComboBox* combo, const std::string& key) {
    return std::max(0, combo->findData(QString::fromStdString(key)));
}

void addPresets(QComboBox* combo) {
    for (const weather::WeatherPreset& p : presets().presets()) {
        combo->addItem(QString::fromStdString(p.name), QString::fromStdString(p.key));
    }
}

QString escaped(const std::string& text) { return QString::fromStdString(text).toHtmlEscaped(); }

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

    auto* camBox = new QGroupBox(tr("Exposure"));
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
    const auto exposureChanged = [this] {
        EnvironmentConfig c = scene_->environment();
        c.exposure.mode = exposureMode_->currentIndex() == 1 ? graphics::ExposureMode::Manual
                                                             : graphics::ExposureMode::Auto;
        c.exposure.manualEv100 = manualEv_->value();
        c.exposure.compensationEv = compensation_->value();
        apply(c);
    };
    connect(exposureMode_, &QComboBox::activated, this, exposureChanged);
    connect(manualEv_, &QDoubleSpinBox::valueChanged, this, exposureChanged);
    connect(compensation_, &QDoubleSpinBox::valueChanged, this, exposureChanged);
    layout->addWidget(camBox);

    // Weather generator (FR-WX-12).
    auto* wxBox = new QGroupBox(tr("Weather"));
    auto* wxForm = new QFormLayout(wxBox);
    preset_ = new QComboBox;
    preset_->setObjectName(QStringLiteral("WeatherPresetCombo"));
    addPresets(preset_);
    preset_->setToolTip(
        tr("Loads the preset's clouds, precipitation, visibility, fog and, when "
           "it sets them, its temperature, humidity and wind."));
    wxForm->addRow(tr("Preset:"), preset_);
    connect(preset_, &QComboBox::activated, this, &EnvironmentPanel::onPresetChosen);

    seed_ = new QSpinBox;
    seed_->setObjectName(QStringLiteral("WeatherSeedSpin"));
    seed_->setRange(0, static_cast<int>(kMaxWeatherSeed));
    seed_->setKeyboardTracking(false);
    seed_->setToolTip(
        tr("Same seed and settings give the same weather (gusts, cloud shapes, "
           "a random wind direction)."));
    randomise_ = new QPushButton(tr("Randomise"));
    randomise_->setObjectName(QStringLiteral("WeatherRandomiseButton"));
    auto* seedRow = new QHBoxLayout;
    seedRow->addWidget(seed_, 1);
    seedRow->addWidget(randomise_);
    wxForm->addRow(tr("Seed:"), seedRow);
    connect(seed_, &QSpinBox::valueChanged, this, &EnvironmentPanel::onWeatherEdited);
    connect(randomise_, &QPushButton::clicked, this, [this] {
        std::random_device device;
        std::uniform_int_distribution<int> pick(1, static_cast<int>(kMaxWeatherSeed));
        seed_->setValue(pick(device));  // Applies through valueChanged.
    });

    auto* cloudGrid = new QGridLayout;
    cloudGrid->addWidget(new QLabel(tr("Type")), 0, 0);
    cloudGrid->addWidget(new QLabel(tr("Oktas")), 0, 1);
    cloudGrid->addWidget(new QLabel(tr("Base")), 0, 2);
    cloudGrid->addWidget(new QLabel(tr("Top")), 0, 3);
    for (std::size_t i = 0; i < clouds_.size(); ++i) {
        CloudRow& r = clouds_[i];
        const int gridRow = static_cast<int>(i) + 1;
        r.type = new QComboBox;
        r.type->setObjectName(QStringLiteral("CloudType%1").arg(i));
        r.type->addItems(
            {tr("Cumulus"), tr("Stratocumulus"), tr("Stratus"), tr("Cirrus"), tr("Cumulonimbus")});
        r.cover = new QSpinBox;
        r.cover->setObjectName(QStringLiteral("CloudCover%1").arg(i));
        r.cover->setRange(0, 8);
        r.cover->setToolTip(tr("Sky cover in eighths (0 = no layer)."));
        r.base = new QSpinBox;
        r.base->setObjectName(QStringLiteral("CloudBase%1").arg(i));
        r.base->setRange(30, 15'000);
        r.base->setSingleStep(100);
        r.base->setSuffix(tr(" m"));
        r.base->setToolTip(tr("Cloud base above the ground at the scene origin."));
        r.top = new QSpinBox;
        r.top->setObjectName(QStringLiteral("CloudTop%1").arg(i));
        r.top->setRange(60, 16'000);
        r.top->setSingleStep(100);
        r.top->setSuffix(tr(" m"));
        for (QSpinBox* spin : {r.cover, r.base, r.top}) {
            spin->setKeyboardTracking(false);
            connect(spin, &QSpinBox::valueChanged, this, &EnvironmentPanel::onWeatherEdited);
        }
        connect(r.type, &QComboBox::activated, this, &EnvironmentPanel::onWeatherEdited);
        cloudGrid->addWidget(r.type, gridRow, 0);
        cloudGrid->addWidget(r.cover, gridRow, 1);
        cloudGrid->addWidget(r.base, gridRow, 2);
        cloudGrid->addWidget(r.top, gridRow, 3);
    }
    cloudGrid->setColumnStretch(0, 1);
    wxForm->addRow(tr("Clouds:"), cloudGrid);

    precipitation_ = new QComboBox;
    precipitation_->setObjectName(QStringLiteral("PrecipitationCombo"));
    precipitation_->addItems({tr("None"), tr("Drizzle"), tr("Rain"), tr("Snow"), tr("Sleet")});
    intensity_ = new QComboBox;
    intensity_->setObjectName(QStringLiteral("IntensityCombo"));
    intensity_->addItems({tr("Light"), tr("Moderate"), tr("Heavy")});
    auto* precipRow = new QHBoxLayout;
    precipRow->addWidget(precipitation_, 1);
    precipRow->addWidget(intensity_, 1);
    wxForm->addRow(tr("Precipitation:"), precipRow);
    connect(precipitation_, &QComboBox::activated, this, &EnvironmentPanel::onWeatherEdited);
    connect(intensity_, &QComboBox::activated, this, &EnvironmentPanel::onWeatherEdited);

    visibility_ = new QDoubleSpinBox;
    visibility_->setObjectName(QStringLiteral("VisibilitySpin"));
    visibility_->setRange(kMinVisibilityM / 1000.0, kMaxVisibilityM / 1000.0);
    visibility_->setDecimals(2);
    visibility_->setSuffix(tr(" km"));
    visibility_->setKeyboardTracking(false);
    visibility_->setToolTip(tr("Meteorological visibility at the ground: haze, fog, rain, snow."));
    wxForm->addRow(tr("Visibility:"), visibility_);
    connect(visibility_, &QDoubleSpinBox::valueChanged, this, &EnvironmentPanel::onWeatherEdited);

    fogTop_ = new QSpinBox;
    fogTop_->setObjectName(QStringLiteral("FogTopSpin"));
    fogTop_->setRange(0, 1000);
    fogTop_->setSingleStep(10);
    fogTop_->setSuffix(tr(" m"));
    fogTop_->setSpecialValueText(tr("no fog layer"));
    fogTop_->setKeyboardTracking(false);
    fogTop_->setToolTip(tr("Top of a ground fog layer; the visibility above it is clear."));
    wxForm->addRow(tr("Fog top:"), fogTop_);
    connect(fogTop_, &QSpinBox::valueChanged, this, &EnvironmentPanel::onWeatherEdited);

    thunderstorm_ = new QCheckBox(tr("Thunderstorm"));
    thunderstorm_->setObjectName(QStringLiteral("ThunderstormCheck"));
    wxForm->addRow(QString(), thunderstorm_);
    connect(thunderstorm_, &QCheckBox::toggled, this, &EnvironmentPanel::onWeatherEdited);

    timeline_ = new QCheckBox(tr("Change to"));
    timeline_->setObjectName(QStringLiteral("TimelineCheck"));
    timeline_->setToolTip(tr("Timeline: the weather changes to this preset during the flight."));
    timelinePreset_ = new QComboBox;
    timelinePreset_->setObjectName(QStringLiteral("TimelinePresetCombo"));
    addPresets(timelinePreset_);
    auto* changeRow = new QHBoxLayout;
    changeRow->addWidget(timeline_);
    changeRow->addWidget(timelinePreset_, 1);
    wxForm->addRow(tr("Timeline:"), changeRow);
    timelineAt_ = new QSpinBox;
    timelineAt_->setObjectName(QStringLiteral("TimelineAtSpin"));
    timelineAt_->setRange(0, 24 * 60);
    timelineAt_->setSuffix(tr(" min"));
    timelineAt_->setKeyboardTracking(false);
    timelineAt_->setToolTip(tr("Minutes after the start of the flight."));
    timelineOver_ = new QSpinBox;
    timelineOver_->setObjectName(QStringLiteral("TimelineOverSpin"));
    timelineOver_->setRange(0, 6 * 60);
    timelineOver_->setSuffix(tr(" min"));
    timelineOver_->setKeyboardTracking(false);
    timelineOver_->setToolTip(tr("Length of the change (0 = sudden)."));
    auto* whenRow = new QHBoxLayout;
    whenRow->addWidget(new QLabel(tr("after")));
    whenRow->addWidget(timelineAt_, 1);
    whenRow->addWidget(new QLabel(tr("over")));
    whenRow->addWidget(timelineOver_, 1);
    wxForm->addRow(QString(), whenRow);
    connect(timeline_, &QCheckBox::toggled, this, &EnvironmentPanel::onWeatherEdited);
    connect(timelinePreset_, &QComboBox::activated, this, &EnvironmentPanel::onWeatherEdited);
    connect(timelineAt_, &QSpinBox::valueChanged, this, &EnvironmentPanel::onWeatherEdited);
    connect(timelineOver_, &QSpinBox::valueChanged, this, &EnvironmentPanel::onWeatherEdited);

    weatherSummary_ = new QLabel;
    weatherSummary_->setObjectName(QStringLiteral("WeatherSummaryLabel"));
    weatherSummary_->setWordWrap(true);
    weatherSummary_->setTextFormat(Qt::RichText);
    wxForm->addRow(weatherSummary_);
    layout->addWidget(wxBox);

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
    {
        // The weather widgets (FR-WX-12).
        std::vector<QSignalBlocker> blockers;
        for (QObject* w : std::initializer_list<QObject*>{
                 preset_, seed_, precipitation_, intensity_, fogTop_, thunderstorm_, timeline_,
                 timelinePreset_, timelineAt_, timelineOver_}) {
            blockers.emplace_back(w);
        }
        for (CloudRow& r : clouds_) {
            for (QObject* w : std::initializer_list<QObject*>{r.type, r.cover, r.base, r.top}) {
                blockers.emplace_back(w);
            }
        }
        const weather::WeatherSettings& fw = env.freeWeather;
        const weather::WeatherOverrides& o = fw.overrides;
        preset_->setCurrentIndex(presetIndex(preset_, fw.preset));
        seed_->setValue(static_cast<int>(std::min(fw.seed, kMaxWeatherSeed)));
        const std::vector<weather::CloudLayer> layers =
            o.clouds.value_or(std::vector<weather::CloudLayer>{});
        for (std::size_t i = 0; i < clouds_.size(); ++i) {
            CloudRow& r = clouds_[i];
            const weather::CloudLayer l =
                i < layers.size() ? layers[i]
                                  : weather::CloudLayer{.type = weather::CloudType::Cumulus,
                                                        .coverOktas = 0.0,
                                                        .baseM = 1000.0 + 2000.0 * double(i),
                                                        .topM = 1500.0 + 2000.0 * double(i)};
            const auto type = std::ranges::find(kCloudTypes, l.type) - kCloudTypes.begin();
            r.type->setCurrentIndex(static_cast<int>(type));
            r.cover->setValue(static_cast<int>(std::lround(l.coverOktas)));
            r.base->setValue(static_cast<int>(std::lround(l.baseM)));
            r.top->setValue(static_cast<int>(std::lround(l.topM)));
        }
        const weather::Precipitation p = o.precipitation.value_or(weather::Precipitation{});
        precipitation_->setCurrentIndex(static_cast<int>(
            std::ranges::find(kPrecipitationTypes, p.type) - kPrecipitationTypes.begin()));
        intensity_->setCurrentIndex(static_cast<int>(
            std::ranges::find(kIntensities, weather::intensityOf(p.type, p.rateMmH)) -
            kIntensities.begin()));
        fogTop_->setValue(static_cast<int>(std::lround(o.fog.value_or(weather::Fog{}).topM)));
        thunderstorm_->setChecked(o.thunderstorm.value_or(false));
        const weather::WeatherChange change =
            fw.timeline.empty() ? defaultChange() : fw.timeline.front();
        timeline_->setChecked(!fw.timeline.empty() && fw.timelineEnabled);
        timelinePreset_->setCurrentIndex(presetIndex(timelinePreset_, change.preset));
        timelineAt_->setValue(static_cast<int>(std::lround(change.atS / 60.0)));
        timelineOver_->setValue(static_cast<int>(std::lround(change.transitionS / 60.0)));
    }
    updateWeatherSummary();
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
    for (CloudRow& r : clouds_) {
        const bool layer = r.cover->value() > 0;
        r.type->setEnabled(layer);
        r.base->setEnabled(layer);
        r.top->setEnabled(layer);
    }
    intensity_->setEnabled(precipitation_->currentIndex() > 0);
    for (QWidget* w :
         std::initializer_list<QWidget*>{timelinePreset_, timelineAt_, timelineOver_}) {
        w->setEnabled(timeline_->isChecked());
    }
}

void EnvironmentPanel::onPresetChosen(int index) {
    const auto& all = presets().presets();
    if (index < 0 || static_cast<std::size_t>(index) >= all.size()) {
        return;
    }
    const weather::WeatherPreset& p = all[static_cast<std::size_t>(index)];
    const weather::WeatherOverrides& v = p.values;
    EnvironmentConfig c = scene_->environment();
    weather::WeatherSettings& fw = c.freeWeather;
    fw.preset = p.key;
    // A preset replaces the whole sky; its air and wind members, when it has them, go to the
    // fields below (FR-WX-01: a preset sets only what it lists).
    fw.overrides.clouds = v.clouds.value_or(std::vector<weather::CloudLayer>{});
    fw.overrides.precipitation = v.precipitation.value_or(weather::Precipitation{});
    fw.overrides.fog = v.fog.value_or(weather::Fog{});
    fw.overrides.thunderstorm = v.thunderstorm.value_or(false);
    c.visibilityM = std::clamp(v.visibilityM.value_or(weather::defaultConditions().visibilityM),
                               kMinVisibilityM, kMaxVisibilityM);
    if (v.temperatureC) {
        c.air.temperatureC = std::clamp(*v.temperatureC, kMinTemperatureC, kMaxTemperatureC);
    }
    if (v.pressureHpa) {
        c.air.pressureHpa = std::clamp(*v.pressureHpa, kMinQnhHpa, kMaxQnhHpa);
    }
    if (v.humidityPct) {
        c.air.humidityPct = std::clamp(*v.humidityPct, 0.0, 100.0);
    } else if (v.dewPointC) {
        c.air.humidityPct = weather::humidityFromDewPoint(c.air.temperatureC, *v.dewPointC);
    }
    if (v.windLevel) {
        c.wind.level = std::clamp(*v.windLevel, 0, weather::WindLevelTable::kLevelCount - 1);
    }
    if (v.windFromDeg) {
        c.wind.fromDeg = *v.windFromDeg;
    }
    apply(c);
}

void EnvironmentPanel::onWeatherEdited() {
    EnvironmentConfig c = scene_->environment();
    weather::WeatherSettings& fw = c.freeWeather;
    weather::WeatherOverrides& o = fw.overrides;
    fw.seed = static_cast<std::uint64_t>(seed_->value());
    std::vector<weather::CloudLayer> layers;
    for (const CloudRow& r : clouds_) {
        if (r.cover->value() > 0) {
            layers.push_back({.type = kCloudTypes[static_cast<std::size_t>(
                                  std::clamp(r.type->currentIndex(), 0, 4))],
                              .coverOktas = static_cast<double>(r.cover->value()),
                              .baseM = static_cast<double>(r.base->value()),
                              .topM = static_cast<double>(r.top->value())});
        }
    }
    std::ranges::stable_sort(layers, {}, &weather::CloudLayer::baseM);
    o.clouds = std::move(layers);
    const auto type = kPrecipitationTypes[static_cast<std::size_t>(
        std::clamp(precipitation_->currentIndex(), 0, 4))];
    const auto intensity =
        kIntensities[static_cast<std::size_t>(std::clamp(intensity_->currentIndex(), 0, 2))];
    o.precipitation =
        weather::Precipitation{type, type == weather::PrecipitationType::None
                                         ? 0.0
                                         : weather::intensityRateMmH(type, intensity)};
    weather::Fog fog = o.fog.value_or(weather::Fog{});
    fog.topM = fogTop_->value();
    o.fog = fog;
    o.thunderstorm = thunderstorm_->isChecked();
    c.visibilityM = visibility_->value() * 1000.0;
    weather::WeatherChange change = fw.timeline.empty() ? defaultChange() : fw.timeline.front();
    change.preset = timelinePreset_->currentData().toString().toStdString();
    change.atS = timelineAt_->value() * 60.0;
    change.transitionS = timelineOver_->value() * 60.0;
    fw.timeline = {change};
    fw.timelineEnabled = timeline_->isChecked();
    apply(c);
    updateEnabled();
}

void EnvironmentPanel::updateWeatherSummary() {
    const EnvironmentConfig& env = scene_->environment();
    // In a mission the panel is read only and shows the mission's weather at its start.
    const weather::WeatherConditions start =
        env.weather ? weather::WeatherGenerator(*env.weather).plannedAt(0.0)
                    : freeWeatherStart(env);
    QString text = escaped(weather::describe(start, weather::WindLevelTable::builtIn()));
    for (const weather::WeatherIssue& issue : weather::checkPlausibility(start)) {
        text += QStringLiteral("<br><span style=\"color:%1\">%2: %3</span>")
                    .arg(issue.error ? QStringLiteral("#d03030") : QStringLiteral("#c08000"),
                         issue.error ? tr("Not plausible") : tr("Unusual"), escaped(issue.message));
    }
    weatherSummary_->setText(text);
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
