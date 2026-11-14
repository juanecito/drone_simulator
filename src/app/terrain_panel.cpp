// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/terrain_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ds/app/scene_controller.hpp"
#include "ds/common/random.hpp"
#include "ds/terrain/procedural_source.hpp"
#include "ds/terrain/query.hpp"

namespace ds::app {
namespace {

constexpr int kPreviewSize = 200;
constexpr double kUnboundedPreviewM = 20'000.0;

QDoubleSpinBox* spin(const char* name, double min, double max, double step, int decimals,
                     const QString& suffix = {}) {
    auto* box = new QDoubleSpinBox;
    box->setObjectName(QLatin1StringView(name));
    box->setRange(min, max);
    box->setSingleStep(step);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    box->setKeyboardTracking(false);
    return box;
}

void disableItem(QComboBox* combo, int index, const QString& tip) {
    if (auto* model = qobject_cast<QStandardItemModel*>(combo->model())) {
        if (QStandardItem* item = model->item(index)) {
            item->setEnabled(false);
            item->setToolTip(tip);
        }
    }
}

// Hypsometric tint: green lowlands → brown hills → grey rock → white peaks.
QRgb heightColor(double t) {
    struct Stop {
        double t;
        int r, g, b;
    };
    static constexpr std::array<Stop, 5> kStops{{{0.0, 70, 120, 60},
                                                 {0.35, 140, 160, 90},
                                                 {0.65, 150, 120, 85},
                                                 {0.85, 130, 125, 120},
                                                 {1.0, 245, 245, 250}}};
    t = std::clamp(t, 0.0, 1.0);
    for (std::size_t i = 1; i < kStops.size(); ++i) {
        if (t <= kStops[i].t) {
            const Stop& a = kStops[i - 1];
            const Stop& b = kStops[i];
            const double f = (t - a.t) / (b.t - a.t);
            return qRgb(static_cast<int>(a.r + f * (b.r - a.r)),
                        static_cast<int>(a.g + f * (b.g - a.g)),
                        static_cast<int>(a.b + f * (b.b - a.b)));
        }
    }
    return qRgb(245, 245, 250);
}

}  // namespace

TerrainPanel::TerrainPanel(SceneController& scene, QWidget* parent)
    : QWidget(parent), scene_(&scene) {
    buildUi();
    reloadPresets();
    setFormConfig(scene.terrain());
    updatePreview();
    connect(scene_, &SceneController::presetsChanged, this, &TerrainPanel::reloadPresets);
}

void TerrainPanel::buildUi() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    // Presets.
    auto* presetBox = new QGroupBox(tr("Preset"));
    auto* presetLayout = new QHBoxLayout(presetBox);
    presets_ = new QComboBox;
    presets_->setObjectName(QStringLiteral("PresetCombo"));
    presets_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* save = new QPushButton(tr("Save As…"));
    save->setObjectName(QStringLiteral("SavePresetButton"));
    deletePreset_ = new QPushButton(tr("Delete"));
    deletePreset_->setObjectName(QStringLiteral("DeletePresetButton"));
    presetLayout->addWidget(presets_);
    presetLayout->addWidget(save);
    presetLayout->addWidget(deletePreset_);
    layout->addWidget(presetBox);
    connect(presets_, &QComboBox::activated, this, &TerrainPanel::onPresetSelected);
    connect(save, &QPushButton::clicked, this, &TerrainPanel::savePreset);
    connect(deletePreset_, &QPushButton::clicked, this, &TerrainPanel::deletePreset);

    // Source and dimensions.
    auto* general = new QGroupBox(tr("Terrain"));
    auto* form = new QFormLayout(general);
    source_ = new QComboBox;
    source_->setObjectName(QStringLiteral("SourceCombo"));
    source_->addItems({tr("Procedural"), tr("Online elevation"), tr("Elevation file")});
    disableItem(source_, 1, tr("Available in milestone M4."));
    disableItem(source_, 2, tr("Available in milestone M4."));
    form->addRow(tr("Source:"), source_);
    dimensions_ = new QComboBox;
    dimensions_->setObjectName(QStringLiteral("DimensionsCombo"));
    dimensions_->addItems({tr("Unlimited"), tr("Custom")});
    form->addRow(tr("Dimensions:"), dimensions_);
    widthKm_ = spin("WidthSpin", 0.256, 200.0, 1.0, 3, tr(" km"));
    lengthKm_ = spin("LengthSpin", 0.256, 200.0, 1.0, 3, tr(" km"));
    auto* size = new QHBoxLayout;
    size->addWidget(widthKm_);
    size->addWidget(new QLabel(QStringLiteral("×")));
    size->addWidget(lengthKm_);
    form->addRow(tr("Width × length:"), size);
    connect(dimensions_, &QComboBox::currentIndexChanged, this, &TerrainPanel::updateEnabled);
    layout->addWidget(general);

    // Procedural parameters.
    auto* proc = new QGroupBox(tr("Procedural"));
    auto* pform = new QFormLayout(proc);
    seed_ = new QLineEdit;
    seed_->setObjectName(QStringLiteral("SeedEdit"));
    seed_->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{1,20}")), seed_));
    auto* random = new QPushButton(tr("Random"));
    random->setObjectName(QStringLiteral("RandomSeedButton"));
    connect(random, &QPushButton::clicked, this,
            [this] { seed_->setText(QString::number(common::randomSeed())); });
    auto* seedRow = new QHBoxLayout;
    seedRow->addWidget(seed_);
    seedRow->addWidget(random);
    pform->addRow(tr("Seed:"), seedRow);
    featureSize_ = spin("FeatureSizeSpin", 50.0, 50'000.0, 100.0, 0, tr(" m"));
    featureSize_->setToolTip(tr("Size of the largest hills."));
    pform->addRow(tr("Feature size:"), featureSize_);
    amplitude_ = spin("AmplitudeSpin", 0.0, 8000.0, 10.0, 0, tr(" m"));
    pform->addRow(tr("Height range:"), amplitude_);
    baseHeight_ = spin("BaseHeightSpin", -2000.0, 6000.0, 10.0, 0, tr(" m"));
    pform->addRow(tr("Base height:"), baseHeight_);
    octaves_ = new QSpinBox;
    octaves_->setObjectName(QStringLiteral("OctavesSpin"));
    octaves_->setRange(1, 16);  // terrain::validate() limits.
    pform->addRow(tr("Detail octaves:"), octaves_);
    lacunarity_ = spin("LacunaritySpin", 1.5, 4.0, 0.1, 2);
    pform->addRow(tr("Lacunarity:"), lacunarity_);
    persistence_ = spin("PersistenceSpin", 0.05, 0.95, 0.05, 2);
    pform->addRow(tr("Roughness:"), persistence_);
    mountains_ = spin("MountainsSpin", 0.0, 1.0, 0.05, 2);
    pform->addRow(tr("Mountains:"), mountains_);
    warp_ = spin("WarpSpin", 0.0, 1.0, 0.05, 2);
    pform->addRow(tr("Warp:"), warp_);
    erosion_ = spin("ErosionSpin", 0.0, 1.0, 0.05, 2);
    pform->addRow(tr("Erosion:"), erosion_);
    water_ = new QCheckBox(tr("Water at"));
    water_->setObjectName(QStringLiteral("WaterCheck"));
    waterLevel_ = spin("WaterLevelSpin", -2000.0, 6000.0, 5.0, 1, tr(" m"));
    auto* waterRow = new QHBoxLayout;
    waterRow->addWidget(water_);
    waterRow->addWidget(waterLevel_);
    pform->addRow(tr("Water:"), waterRow);
    connect(water_, &QCheckBox::toggled, this, &TerrainPanel::updateEnabled);
    edge_ = new QComboBox;
    edge_->setObjectName(QStringLiteral("EdgeCombo"));
    edge_->addItem(tr("Cliff"), static_cast<int>(terrain::EdgeMode::Cliff));
    edge_->addItem(tr("Fade to base height"), static_cast<int>(terrain::EdgeMode::Fade));
    edge_->addItem(tr("Water"), static_cast<int>(terrain::EdgeMode::Water));
    pform->addRow(tr("Edge:"), edge_);
    layout->addWidget(proc);

    // Origin.
    auto* origin = new QGroupBox(tr("Geographic origin"));
    auto* oform = new QFormLayout(origin);
    latitude_ = spin("LatitudeSpin", -89.0, 89.0, 0.1, 6, QStringLiteral("°"));
    longitude_ = spin("LongitudeSpin", -180.0, 180.0, 0.1, 6, QStringLiteral("°"));
    altitude_ = spin("AltitudeSpin", -500.0, 9000.0, 10.0, 1, tr(" m AMSL"));
    oform->addRow(tr("Latitude:"), latitude_);
    oform->addRow(tr("Longitude:"), longitude_);
    oform->addRow(tr("Altitude:"), altitude_);
    layout->addWidget(origin);

    auto* regenerate = new QPushButton(tr("Regenerate"));
    regenerate->setObjectName(QStringLiteral("RegenerateButton"));
    regenerate->setDefault(true);
    connect(regenerate, &QPushButton::clicked, this, [this] { this->regenerate(); });
    layout->addWidget(regenerate);
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("StatusLabel"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    preview_ = new QLabel;
    preview_->setObjectName(QStringLiteral("PreviewLabel"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(kPreviewSize, kPreviewSize);
    preview_->setToolTip(tr("Height preview (north up). The cross marks the origin."));
    layout->addWidget(preview_);
    layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

void TerrainPanel::reloadPresets() {
    const QSignalBlocker blocker(presets_);
    presets_->clear();
    presets_->addItem(tr("(custom)"), QString());
    for (const PresetInfo& preset : scene_->presets()) {
        presets_->addItem(preset.builtIn ? preset.name : tr("%1 (mine)").arg(preset.name),
                          preset.key);
    }
    const int index = presets_->findData(QString::fromStdString(scene_->terrain().presetKey));
    presets_->setCurrentIndex(std::max(index, 0));
    updateEnabled();
}

void TerrainPanel::onPresetSelected(int index) {
    const QString key = presets_->itemData(index).toString();
    if (key.isEmpty()) {
        return;
    }
    auto config = scene_->presetConfig(key);
    if (!config) {
        showStatus(QString::fromStdString(config.error().message), true);
        return;
    }
    setFormConfig(*config);
    regenerate();
}

void TerrainPanel::savePreset() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Terrain Preset"), tr("Name:"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    TerrainConfig config = formConfig();
    if (auto valid = terrain::validate(config.params); !valid) {
        showStatus(QString::fromStdString(valid.error().message), true);
        return;
    }
    auto key = scene_->savePreset(name, config);
    if (!key) {
        showStatus(QString::fromStdString(key.error().message), true);
        return;
    }
    presets_->setCurrentIndex(std::max(presets_->findData(*key), 0));
    showStatus(tr("Preset “%1” saved.").arg(name.trimmed()), false);
}

void TerrainPanel::deletePreset() {
    const QString key = presets_->currentData().toString();
    if (!key.startsWith(QLatin1String("user."))) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Delete Preset"), tr("Delete the preset “%1”?").arg(presets_->currentText()));
    if (answer != QMessageBox::Yes) {
        return;
    }
    if (auto removed = scene_->deletePreset(key); !removed) {
        showStatus(QString::fromStdString(removed.error().message), true);
    }
}

TerrainConfig TerrainPanel::formConfig() const {
    TerrainConfig c;
    c.presetKey = presets_->currentData().toString().toStdString();
    terrain::ProceduralParams& p = c.params;
    p = hidden_;
    p.seed = seed_->text().toULongLong();
    p.featureSizeM = featureSize_->value();
    p.amplitudeM = amplitude_->value();
    p.baseHeightM = baseHeight_->value();
    p.octaves = octaves_->value();
    p.lacunarity = lacunarity_->value();
    p.persistence = persistence_->value();
    p.mountains = mountains_->value();
    p.warp = warp_->value();
    p.erosion = erosion_->value();
    p.waterLevelM =
        water_->isChecked() ? std::optional<double>(waterLevel_->value()) : std::nullopt;
    p.edge = static_cast<terrain::EdgeMode>(edge_->currentData().toInt());
    if (dimensions_->currentIndex() == 1) {
        c.extent = terrain::Extent{widthKm_->value() * 1000.0, lengthKm_->value() * 1000.0};
    }
    c.origin = {latitude_->value(), longitude_->value(), altitude_->value()};
    return c;
}

void TerrainPanel::setFormConfig(const TerrainConfig& c) {
    const terrain::ProceduralParams& p = c.params;
    hidden_ = p;
    seed_->setText(QString::number(p.seed));
    featureSize_->setValue(p.featureSizeM);
    amplitude_->setValue(p.amplitudeM);
    baseHeight_->setValue(p.baseHeightM);
    octaves_->setValue(p.octaves);
    lacunarity_->setValue(p.lacunarity);
    persistence_->setValue(p.persistence);
    mountains_->setValue(p.mountains);
    warp_->setValue(p.warp);
    erosion_->setValue(p.erosion);
    water_->setChecked(p.waterLevelM.has_value());
    waterLevel_->setValue(p.waterLevelM.value_or(0.0));
    edge_->setCurrentIndex(std::max(edge_->findData(static_cast<int>(p.edge)), 0));
    dimensions_->setCurrentIndex(c.extent ? 1 : 0);
    widthKm_->setValue(c.extent ? c.extent->widthM / 1000.0 : 10.0);
    lengthKm_->setValue(c.extent ? c.extent->lengthM / 1000.0 : 10.0);
    latitude_->setValue(c.origin.latitudeDeg);
    longitude_->setValue(c.origin.longitudeDeg);
    altitude_->setValue(c.origin.heightM);
    const int index = presets_->findData(QString::fromStdString(c.presetKey));
    presets_->setCurrentIndex(std::max(index, 0));
    updateEnabled();
}

bool TerrainPanel::regenerate() {
    TerrainConfig config = formConfig();
    // Edited parameters no longer match the preset: the terrain becomes "(custom)".
    if (!config.presetKey.empty()) {
        const auto preset = scene_->presetConfig(QString::fromStdString(config.presetKey));
        if (!preset || !(*preset == config)) {
            config.presetKey.clear();
            const QSignalBlocker blocker(presets_);
            presets_->setCurrentIndex(0);
            updateEnabled();
        }
    }
    if (auto applied = scene_->setTerrain(config); !applied) {
        showStatus(QString::fromStdString(applied.error().message), true);
        return false;
    }
    showStatus(tr("Terrain generated."), false);
    updatePreview();
    return true;
}

void TerrainPanel::updateEnabled() {
    const bool custom = dimensions_->currentIndex() == 1;
    widthKm_->setEnabled(custom);
    lengthKm_->setEnabled(custom);
    edge_->setEnabled(custom);
    waterLevel_->setEnabled(water_->isChecked());
    deletePreset_->setEnabled(
        presets_->currentData().toString().startsWith(QLatin1String("user.")));
}

void TerrainPanel::showStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

void TerrainPanel::updatePreview() {
    preview_->setPixmap(QPixmap::fromImage(renderPreview(scene_->terrain(), kPreviewSize)));
}

QImage TerrainPanel::renderPreview(const TerrainConfig& config, int size) {
    QImage image(size, size, QImage::Format_RGB32);
    auto source = terrain::ProceduralSource::create(config.params, config.extent);
    if (!source) {
        image.fill(Qt::darkGray);
        return image;
    }
    const double extentM = config.extent ? std::max(config.extent->widthM, config.extent->lengthM)
                                         : kUnboundedPreviewM;
    const std::vector<float> heights =
        terrain::sampleHeightMap(**source, 0.0, 0.0, extentM, size, size);
    const auto [lo, hi] = std::ranges::minmax_element(heights);
    const double minH = *lo;
    const double range = std::max(static_cast<double>(*hi) - minH, 1.0);
    const double cell = extentM / size;
    const std::optional<double> water = config.params.waterLevelM;
    for (int y = 0; y < size; ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size; ++x) {
            const double h = heights[static_cast<std::size_t>(y * size + x)];
            const double wx =
                x + 1 < size
                    ? static_cast<double>(heights[static_cast<std::size_t>(y * size + x + 1)])
                    : h;
            const double wy =
                y + 1 < size
                    ? static_cast<double>(heights[static_cast<std::size_t>((y + 1) * size + x)])
                    : h;
            // Hill shading lit from the north-west.
            const double nx = (h - wx) / cell;
            const double ny = (h - wy) / cell;
            const double shade =
                std::clamp(0.75 + 0.6 * (nx + ny) / std::sqrt(nx * nx + ny * ny + 1.0), 0.35, 1.25);
            QRgb c = heightColor((h - minH) / range);
            if (water && h < *water) {
                const double depth = std::clamp((*water - h) / 50.0, 0.0, 1.0);
                c = qRgb(static_cast<int>(60 - 30 * depth), static_cast<int>(110 - 40 * depth),
                         static_cast<int>(170 - 40 * depth));
                row[x] = c;
                continue;
            }
            row[x] = qRgb(std::min(255, static_cast<int>(qRed(c) * shade)),
                          std::min(255, static_cast<int>(qGreen(c) * shade)),
                          std::min(255, static_cast<int>(qBlue(c) * shade)));
        }
    }
    // Origin marker.
    const int c = size / 2;
    for (int d = -4; d <= 4; ++d) {
        image.setPixel(c + d, c, qRgb(220, 30, 30));
        image.setPixel(c, c + d, qRgb(220, 30, 30));
    }
    return image;
}

}  // namespace ds::app
