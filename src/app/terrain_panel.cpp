// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/terrain_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ds/app/data_services.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/common/random.hpp"
#include "ds/terrain/online_imagery.hpp"
#include "ds/terrain/procedural_source.hpp"
#include "ds/terrain/query.hpp"
#include "ds/terrain/terrain_files.hpp"

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
    updateMetadata();
    updatePreview();
    connect(scene_, &SceneController::presetsChanged, this, &TerrainPanel::reloadPresets);
    connect(scene_, &SceneController::terrainChanged, this, [this] {
        updateMetadata();
        updatePreview();
    });
    // Streaming online terrain: blocks arrive in the background.
    refresh_ = new QTimer(this);
    refresh_->setInterval(2000);
    connect(refresh_, &QTimer::timeout, this, [this] {
        if (isVisible() &&
            (scene_->onlineStatus() ||
             dynamic_cast<const terrain::OnlineImagerySource*>(scene_->orthophotos().get()))) {
            updateMetadata();
            updateQuota();
        }
    });
    refresh_->start();
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
    if (scene_->dataServices() == nullptr || scene_->dataServices()->elevationProviders().empty()) {
        disableItem(source_, 1, tr("No online elevation provider is available."));
    }
    form->addRow(tr("Source:"), source_);
    connect(source_, &QComboBox::currentIndexChanged, this, &TerrainPanel::updateEnabled);
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

    // Elevation files (FR-TER-F11).
    fileBox_ = new QGroupBox(tr("Elevation files"));
    fileBox_->setObjectName(QStringLiteral("FileBox"));
    auto* fform = new QFormLayout(fileBox_);
    files_ = new QListWidget;
    files_->setObjectName(QStringLiteral("FileList"));
    files_->setMaximumHeight(90);
    files_->setToolTip(
        tr("Elevation rasters (COG, GeoTIFF, .hgt, .asc, .img) or classified lidar point "
           "clouds (.las, .laz) in one CRS; several tiles are joined into one surface."));
    fform->addRow(files_);
    auto* addFile = new QPushButton(tr("Add…"));
    addFile->setObjectName(QStringLiteral("AddFileButton"));
    removeFile_ = new QPushButton(tr("Remove"));
    removeFile_->setObjectName(QStringLiteral("RemoveFileButton"));
    auto* fileButtons = new QHBoxLayout;
    fileButtons->addWidget(addFile);
    fileButtons->addWidget(removeFile_);
    fileButtons->addStretch();
    fform->addRow(fileButtons);
    connect(addFile, &QPushButton::clicked, this, &TerrainPanel::addFiles);
    connect(removeFile_, &QPushButton::clicked, this, [this] {
        qDeleteAll(files_->selectedItems());
        updateEnabled();
    });
    connect(files_, &QListWidget::itemSelectionChanged, this, &TerrainPanel::updateEnabled);
    band_ = new QSpinBox;
    band_->setObjectName(QStringLiteral("BandSpin"));
    band_->setRange(1, 1000);
    bandLabel_ = new QLabel(tr("Band:"));
    fform->addRow(bandLabel_, band_);
    // LAS / LAZ: the ground points become a DTM (FR-TER-L02, L03).
    lidarBox_ = new QWidget;
    lidarBox_->setObjectName(QStringLiteral("LidarBox"));
    auto* lform = new QFormLayout(lidarBox_);
    lform->setContentsMargins(0, 0, 0, 0);
    lidarAuto_ = new QCheckBox(tr("Automatic"));
    lidarAuto_->setObjectName(QStringLiteral("LidarAutoCheck"));
    lidarAuto_->setChecked(true);
    lidarAuto_->setToolTip(tr("About two point spacings, 0.5 … 20 m"));
    lidarResolution_ = new QDoubleSpinBox;
    lidarResolution_->setObjectName(QStringLiteral("LidarResolutionSpin"));
    lidarResolution_->setRange(0.25, 100.0);
    lidarResolution_->setSingleStep(0.25);
    lidarResolution_->setValue(1.0);
    lidarResolution_->setSuffix(tr(" m"));
    auto* resolution = new QHBoxLayout;
    resolution->addWidget(lidarAuto_);
    resolution->addWidget(lidarResolution_);
    lform->addRow(tr("DTM cell size:"), resolution);
    lidarInterpolation_ = new QComboBox;
    lidarInterpolation_->setObjectName(QStringLiteral("LidarInterpolationCombo"));
    lidarInterpolation_->addItem(tr("Triangulation (TIN)"),
                                 static_cast<int>(terrain::LidarInterpolation::Tin));
    lidarInterpolation_->addItem(tr("Inverse distance (IDW)"),
                                 static_cast<int>(terrain::LidarInterpolation::Idw));
    lidarInterpolation_->setToolTip(
        tr("Fills gaps up to %1 m across; larger gaps stay without data")
            .arg(terrain::kLidarMaxHoleM));
    lform->addRow(tr("Gap filling:"), lidarInterpolation_);
    fform->addRow(lidarBox_);
    connect(lidarAuto_, &QCheckBox::toggled, this, &TerrainPanel::updateEnabled);
    crs_ = new QLineEdit;
    crs_->setObjectName(QStringLiteral("CrsEdit"));
    crs_->setPlaceholderText(tr("from the files, e.g. EPSG:25830"));
    fform->addRow(tr("CRS:"), crs_);
    datum_ = new QLineEdit;
    datum_->setObjectName(QStringLiteral("DatumEdit"));
    datum_->setPlaceholderText(tr("from the files, e.g. EGM2008"));
    fform->addRow(tr("Vertical datum:"), datum_);
    attribution_ = new QLineEdit;
    attribution_->setObjectName(QStringLiteral("AttributionEdit"));
    attribution_->setPlaceholderText(tr("dataset licence / citation, shown in the view"));
    fform->addRow(tr("Attribution:"), attribution_);
    fileEdge_ = new QComboBox;
    fileEdge_->setObjectName(QStringLiteral("FileEdgeCombo"));
    fileEdge_->addItem(tr("Clamp edge"), static_cast<int>(terrain::EdgeMode::Cliff));
    fileEdge_->addItem(tr("Fade to lowest height"), static_cast<int>(terrain::EdgeMode::Fade));
    fform->addRow(tr("Edge:"), fileEdge_);
    layout->addWidget(fileBox_);

    // Online elevation (FR-TER-O01 … O18).
    onlineBox_ = new QGroupBox(tr("Online elevation"));
    onlineBox_->setObjectName(QStringLiteral("OnlineBox"));
    auto* oform2 = new QFormLayout(onlineBox_);
    provider_ = new QComboBox;
    provider_->setObjectName(QStringLiteral("ProviderCombo"));
    oform2->addRow(tr("Provider:"), provider_);
    dataset_ = new QComboBox;
    dataset_->setObjectName(QStringLiteral("DatasetCombo"));
    oform2->addRow(tr("Dataset:"), dataset_);
    onlineEdge_ = new QComboBox;
    onlineEdge_->setObjectName(QStringLiteral("OnlineEdgeCombo"));
    onlineEdge_->addItem(tr("Clamp edge"), static_cast<int>(terrain::EdgeMode::Cliff));
    onlineEdge_->addItem(tr("Fade to lowest height"), static_cast<int>(terrain::EdgeMode::Fade));
    oform2->addRow(tr("Edge:"), onlineEdge_);
    download_ = new QPushButton(tr("Download Area…"));
    download_->setObjectName(QStringLiteral("DownloadAreaButton"));
    download_->setToolTip(
        tr("Downloads the terrain area (with custom dimensions) or the block around the origin "
           "into the cache, for offline use."));
    oform2->addRow(download_);
    quota_ = new QLabel;
    quota_->setObjectName(QStringLiteral("QuotaLabel"));
    quota_->setWordWrap(true);
    oform2->addRow(quota_);
    auto* privacy = new QLabel(tr("The requested area is visible to the provider."));
    privacy->setWordWrap(true);
    privacy->setEnabled(false);
    oform2->addRow(privacy);
    layout->addWidget(onlineBox_);
    connect(provider_, &QComboBox::currentIndexChanged, this, [this] {
        fillDatasets();
        updateQuota();
    });
    connect(download_, &QPushButton::clicked, this, [this] { downloadArea(); });
    fillProviders();
    if (DataServices* services = scene_->dataServices()) {
        // Settings › Data providers: providers added or removed, keys stored, offline mode.
        connect(services, &DataServices::providersChanged, this, [this, services] {
            const QString provider = provider_->currentData().toString();
            const QString dataset = dataset_->currentData().toString();
            fillProviders();
            if (const int i = provider_->findData(provider); i >= 0) {
                provider_->setCurrentIndex(i);
                fillDatasets();
                dataset_->setCurrentIndex(std::max(dataset_->findData(dataset), 0));
            }
            fillImageryProviders();
            if (!services->elevationProviders().empty()) {
                if (auto* model = qobject_cast<QStandardItemModel*>(source_->model())) {
                    model->item(1)->setEnabled(true);
                    model->item(1)->setToolTip({});
                }
            }
            updateQuota();
        });
        connect(services, &DataServices::apiKeysChanged, this, &TerrainPanel::updateQuota);
        connect(services, &DataServices::networkSettingsChanged, this, &TerrainPanel::updateQuota);
    }

    // Orthophotos over file or online elevation (FR-TER-I10).
    orthoBox_ = new QGroupBox(tr("Imagery"));
    orthoBox_->setObjectName(QStringLiteral("OrthoBox"));
    auto* iform = new QFormLayout(orthoBox_);
    orthoForm_ = iform;
    orthoSource_ = new QComboBox;
    orthoSource_->setObjectName(QStringLiteral("OrthoSourceCombo"));
    orthoSource_->setToolTip(
        tr("Orthophoto files, or an online imagery provider (Settings › Data providers)"));
    iform->addRow(tr("Source:"), orthoSource_);
    orthoLayer_ = new QComboBox;
    orthoLayer_->setObjectName(QStringLiteral("OrthoLayerCombo"));
    iform->addRow(tr("Layer:"), orthoLayer_);
    connect(orthoSource_, &QComboBox::currentIndexChanged, this, [this] {
        fillImageryLayers();
        updateEnabled();
    });
    orthoFiles_ = new QListWidget;
    orthoFiles_->setObjectName(QStringLiteral("OrthoList"));
    orthoFiles_->setMaximumHeight(70);
    orthoFiles_->setToolTip(
        tr("Aerial images draped over the terrain: 8-bit RGB or RGBA COG / GeoTIFF, JPEG 2000, "
           "or PNG / JPEG with a world file, in one CRS. Large files without overviews are "
           "converted once to a cached COG."));
    iform->addRow(orthoFiles_);
    auto* addOrtho = new QPushButton(tr("Add…"));
    addOrtho->setObjectName(QStringLiteral("AddOrthoButton"));
    removeOrtho_ = new QPushButton(tr("Remove"));
    removeOrtho_->setObjectName(QStringLiteral("RemoveOrthoButton"));
    auto* orthoButtons = new QHBoxLayout;
    orthoButtons_ = orthoButtons;
    orthoButtons->addWidget(addOrtho);
    orthoButtons->addWidget(removeOrtho_);
    orthoButtons->addStretch();
    iform->addRow(orthoButtons);
    connect(addOrtho, &QPushButton::clicked, this, &TerrainPanel::addOrthoFiles);
    connect(removeOrtho_, &QPushButton::clicked, this, [this] {
        qDeleteAll(orthoFiles_->selectedItems());
        updateEnabled();
    });
    connect(orthoFiles_, &QListWidget::itemSelectionChanged, this, &TerrainPanel::updateEnabled);
    orthoCrs_ = new QLineEdit;
    orthoCrs_->setObjectName(QStringLiteral("OrthoCrsEdit"));
    orthoCrs_->setPlaceholderText(tr("from the files, e.g. EPSG:25830"));
    iform->addRow(tr("CRS:"), orthoCrs_);
    orthoBands_ = new QLineEdit;
    orthoBands_->setObjectName(QStringLiteral("OrthoBandsEdit"));
    orthoBands_->setPlaceholderText(tr("automatic, or e.g. 1, 2, 3"));
    orthoBands_->setToolTip(tr("Red, green, blue (and alpha) bands of multi-band images"));
    iform->addRow(tr("Bands:"), orthoBands_);
    orthoAttribution_ = new QLineEdit;
    orthoAttribution_->setObjectName(QStringLiteral("OrthoAttributionEdit"));
    orthoAttribution_->setPlaceholderText(tr("imagery licence / citation, shown in the view"));
    iform->addRow(tr("Attribution:"), orthoAttribution_);
    auto colourSpin = [](const char* name, double lo, double hi, double value) {
        auto* spin = new QDoubleSpinBox;
        spin->setObjectName(QString::fromLatin1(name));
        spin->setRange(lo, hi);
        spin->setSingleStep(0.05);
        spin->setDecimals(2);
        spin->setValue(value);
        return spin;
    };
    orthoBrightness_ = colourSpin("OrthoBrightnessSpin", -1.0, 1.0, 0.0);
    orthoContrast_ = colourSpin("OrthoContrastSpin", 0.0, 4.0, 1.0);
    orthoGamma_ = colourSpin("OrthoGammaSpin", 0.1, 10.0, 1.0);
    orthoBrightness_->setToolTip(tr("Brightness (0 = unchanged)"));
    orthoContrast_->setToolTip(tr("Contrast (1 = unchanged)"));
    orthoGamma_->setToolTip(tr("Gamma (1 = unchanged)"));
    auto* colour = new QHBoxLayout;
    colour->addWidget(orthoBrightness_);
    colour->addWidget(orthoContrast_);
    colour->addWidget(orthoGamma_);
    iform->addRow(tr("Brightness, contrast, gamma:"), colour);
    orthoInfo_ = new QLabel;
    orthoInfo_->setObjectName(QStringLiteral("OrthoInfoLabel"));
    orthoInfo_->setWordWrap(true);
    orthoInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    iform->addRow(orthoInfo_);
    layout->addWidget(orthoBox_);
    fillImageryProviders();

    metadata_ = new QLabel;
    metadata_->setObjectName(QStringLiteral("MetadataLabel"));
    metadata_->setWordWrap(true);
    metadata_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(metadata_);

    // Procedural parameters.
    auto* proc = new QGroupBox(tr("Procedural"));
    procedural_ = proc;
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
    autoOrigin_ = new QCheckBox(tr("Centre of the data, at ground level"));
    autoOrigin_->setObjectName(QStringLiteral("AutoOriginCheck"));
    autoOrigin_->setChecked(true);
    oform->addRow(autoOrigin_);
    connect(autoOrigin_, &QCheckBox::toggled, this, &TerrainPanel::updateEnabled);
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
    if (source_->currentIndex() == 2) {
        c.kind = TerrainKind::File;
        FileTerrainConfig& f = c.file;
        for (int i = 0; i < files_->count(); ++i) {
            f.files.emplace_back(files_->item(i)->data(Qt::UserRole).toString().toStdString());
        }
        f.band = band_->value();
        f.crs = crs_->text().trimmed().toStdString();
        f.verticalDatum = datum_->text().trimmed().toStdString();
        f.attribution = attribution_->text().trimmed().toStdString();
        f.edge = static_cast<terrain::EdgeMode>(fileEdge_->currentData().toInt());
        if (isPointCloud(f)) {
            f.band = 1;
            if (!lidarAuto_->isChecked()) {
                f.resolutionM = lidarResolution_->value();
            }
            f.interpolation = static_cast<terrain::LidarInterpolation>(
                lidarInterpolation_->currentData().toInt());
        }
        if (!autoOrigin_->isChecked()) {
            f.origin = geo::LatLon{latitude_->value(), longitude_->value()};
            f.originAltitudeM = altitude_->value();
        }
    } else if (source_->currentIndex() == 1) {
        c.kind = TerrainKind::Online;
        OnlineTerrainConfig& o = c.online;
        o.provider = provider_->currentData().toString().toStdString();
        o.dataset = dataset_->currentData().toString().toStdString();
        o.edge = static_cast<terrain::EdgeMode>(onlineEdge_->currentData().toInt());
        if (!autoOrigin_->isChecked()) {
            o.originAltitudeM = altitude_->value();
        }
    }
    if (c.kind != TerrainKind::Procedural && orthoSource_->currentIndex() > 0) {
        c.ortho.provider = orthoSource_->currentData().toString().toStdString();
        c.ortho.dataset = orthoLayer_->currentData().toString().toStdString();
        c.ortho.color = {.brightness = orthoBrightness_->value(),
                         .contrast = orthoContrast_->value(),
                         .gamma = orthoGamma_->value()};
    } else if (c.kind != TerrainKind::Procedural) {
        OrthoConfig& o = c.ortho;
        for (int i = 0; i < orthoFiles_->count(); ++i) {
            o.files.emplace_back(orthoFiles_->item(i)->data(Qt::UserRole).toString().toStdString());
        }
        o.crs = orthoCrs_->text().trimmed().toStdString();
        for (const QString& part : orthoBands_->text().split(
                 QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
            o.bands.push_back(part.toInt());  // Checked when the imagery is opened.
        }
        o.attribution = orthoAttribution_->text().trimmed().toStdString();
        o.color = {.brightness = orthoBrightness_->value(),
                   .contrast = orthoContrast_->value(),
                   .gamma = orthoGamma_->value()};
    }
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
    source_->setCurrentIndex(c.kind == TerrainKind::File     ? 2
                             : c.kind == TerrainKind::Online ? 1
                                                             : 0);
    if (c.kind == TerrainKind::Online) {
        if (const int i = provider_->findData(QString::fromStdString(c.online.provider)); i >= 0) {
            provider_->setCurrentIndex(i);
        }
        fillDatasets();
        if (const int i = dataset_->findData(QString::fromStdString(c.online.dataset)); i >= 0) {
            dataset_->setCurrentIndex(i);
        }
        onlineEdge_->setCurrentIndex(
            std::max(onlineEdge_->findData(static_cast<int>(c.online.edge)), 0));
    }
    files_->clear();
    for (const auto& file : c.file.files) {
        const QString path = QString::fromStdString(file.string());
        auto* item = new QListWidgetItem(QString::fromStdString(file.filename().string()));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        files_->addItem(item);
    }
    band_->setValue(c.file.band);
    crs_->setText(QString::fromStdString(c.file.crs));
    datum_->setText(QString::fromStdString(c.file.verticalDatum));
    attribution_->setText(QString::fromStdString(c.file.attribution));
    fileEdge_->setCurrentIndex(std::max(fileEdge_->findData(static_cast<int>(c.file.edge)), 0));
    lidarAuto_->setChecked(!c.file.resolutionM);
    if (c.file.resolutionM) {
        lidarResolution_->setValue(*c.file.resolutionM);
    }
    lidarInterpolation_->setCurrentIndex(
        std::max(lidarInterpolation_->findData(static_cast<int>(c.file.interpolation)), 0));
    fillImageryProviders();
    if (!c.ortho.provider.empty()) {
        const QString key = QString::fromStdString(c.ortho.provider);
        int i = orthoSource_->findData(key);
        if (i < 0) {  // Not configured here: kept, so the scene is not changed silently.
            orthoSource_->addItem(tr("%1 (not available)").arg(key), key);
            i = orthoSource_->count() - 1;
        }
        orthoSource_->setCurrentIndex(i);
        fillImageryLayers();
        if (const int l = orthoLayer_->findData(QString::fromStdString(c.ortho.dataset)); l >= 0) {
            orthoLayer_->setCurrentIndex(l);
        }
    } else {
        orthoSource_->setCurrentIndex(0);
    }
    orthoFiles_->clear();
    for (const auto& file : c.ortho.files) {
        const QString path = QString::fromStdString(file.string());
        auto* item = new QListWidgetItem(QString::fromStdString(file.filename().string()));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        orthoFiles_->addItem(item);
    }
    orthoCrs_->setText(QString::fromStdString(c.ortho.crs));
    QStringList bands;
    for (const int b : c.ortho.bands) {
        bands << QString::number(b);
    }
    orthoBands_->setText(bands.join(QStringLiteral(", ")));
    orthoAttribution_->setText(QString::fromStdString(c.ortho.attribution));
    orthoBrightness_->setValue(c.ortho.color.brightness);
    orthoContrast_->setValue(c.ortho.color.contrast);
    orthoGamma_->setValue(c.ortho.color.gamma);
    autoOrigin_->setChecked((c.kind == TerrainKind::File && !c.file.origin) ||
                            (c.kind == TerrainKind::Online && !c.online.originAltitudeM));
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
    if (config.kind == TerrainKind::Online && !ensureDownloaded(config)) {
        return false;
    }
    if (config.kind == TerrainKind::File && isPointCloud(config.file) && !ensureLidarDem(config)) {
        return false;
    }
    // Large orthophotos without overviews: a cached COG first (FR-TER-I06). A problem only
    // disables the imagery (FR-TER-I08).
    QString orthoProblem;
    if (config.kind != TerrainKind::Procedural && !config.ortho.files.empty() &&
        config.ortho.provider.empty()) {
        if (DataServices* services = scene_->dataServices()) {
            auto cached = ensureOrthoCache(this, *services, config.ortho.files);
            if (!cached) {
                orthoProblem = QString::fromStdString(cached.error().message);
            } else if (!*cached) {
                showStatus(tr("Cancelled."), true);
                return false;
            }
        }
    }
    if (auto applied = scene_->setTerrain(config); !applied) {
        showStatus(QString::fromStdString(applied.error().message), true);
        return false;
    }
    if (orthoProblem.isEmpty()) {
        orthoProblem = QString::fromStdString(scene_->orthoWarning());
    }
    const QString loaded = config.kind == TerrainKind::File     ? tr("Elevation files loaded.")
                           : config.kind == TerrainKind::Online ? tr("Online elevation loaded.")
                                                                : tr("Terrain generated.");
    showStatus(orthoProblem.isEmpty() ? loaded : tr("%1 Orthophotos: %2").arg(loaded, orthoProblem),
               !orthoProblem.isEmpty());
    if (config.kind != TerrainKind::Procedural) {
        // The origin found in the data.
        const geo::Geodetic& o = scene_->terrain().origin;
        latitude_->setValue(o.latitudeDeg);
        longitude_->setValue(o.longitudeDeg);
        altitude_->setValue(o.heightM);
    }
    updateMetadata();
    updateQuota();
    updatePreview();
    return true;
}

bool TerrainPanel::ensureDownloaded(const TerrainConfig& config) {
    DataServices* services = scene_->dataServices();
    if (services == nullptr) {
        showStatus(tr("Online elevation data is not available here."), true);
        return false;
    }
    auto request = onlineRequest(config, *services);
    if (!request) {
        showStatus(QString::fromStdString(request.error().message), true);
        return false;
    }
    auto cached = ensureOnlineData(this, *services, *request);
    updateQuota();
    if (!cached) {
        showStatus(QString::fromStdString(cached.error().message), true);
        return false;
    }
    if (!*cached) {
        showStatus(tr("Download cancelled."), true);
        return false;
    }
    return true;
}

bool TerrainPanel::ensureLidarDem(const TerrainConfig& config) {
    DataServices* services = scene_->dataServices();
    if (services == nullptr) {
        showStatus(tr("LAS/LAZ terrain needs the data cache, not available here."), true);
        return false;
    }
    auto made = app::ensureLidarDem(this, *services, config.file.files, lidarOptions(config.file));
    if (!made) {
        showStatus(QString::fromStdString(made.error().message), true);
        return false;
    }
    if (!*made) {
        showStatus(tr("Cancelled."), true);
        return false;
    }
    return true;
}

bool TerrainPanel::filesArePointClouds() const {
    return files_->count() > 0 &&
           terrain::elevationFormatOf(
               files_->item(0)->data(Qt::UserRole).toString().toStdString()) ==
               terrain::ElevationFormat::PointCloud;
}

bool TerrainPanel::downloadArea() {
    const TerrainConfig config = formConfig();
    if (config.kind != TerrainKind::Online) {
        return false;
    }
    if (!ensureDownloaded(config)) {
        return false;
    }
    showStatus(tr("The area is in the cache (also offline)."), false);
    return true;
}

void TerrainPanel::fillProviders() {
    const QSignalBlocker blocker(provider_);
    provider_->clear();
    if (DataServices* services = scene_->dataServices()) {
        for (const net::ProviderConfig* p : services->elevationProviders()) {
            provider_->addItem(QString::fromStdString(p->name), QString::fromStdString(p->key));
        }
    }
    fillDatasets();
    updateQuota();
}

void TerrainPanel::fillDatasets() {
    const QSignalBlocker blocker(dataset_);
    dataset_->clear();
    DataServices* services = scene_->dataServices();
    const net::ProviderConfig* p =
        services != nullptr ? services->provider(provider_->currentData().toString().toStdString())
                            : nullptr;
    if (p == nullptr) {
        return;
    }
    for (const net::DatasetConfig& d : p->datasets) {
        dataset_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
        const int i = dataset_->count() - 1;
        dataset_->setItemData(
            i,
            tr("%1 m · %2%3")
                .arg(d.resolutionM, 0, 'g', 3)
                .arg(d.verticalDatum.empty() ? tr("datum unknown")
                                             : QString::fromStdString(d.verticalDatum))
                .arg(d.verified ? QString() : tr(" · request format not verified yet")),
            Qt::ToolTipRole);
    }
    dataset_->setCurrentIndex(
        std::max(dataset_->findData(QString::fromStdString(p->defaultDataset)), 0));
}

void TerrainPanel::updateQuota() {
    DataServices* services = scene_->dataServices();
    const net::ProviderConfig* p =
        services != nullptr ? services->provider(provider_->currentData().toString().toStdString())
                            : nullptr;
    if (p == nullptr) {
        quota_->clear();
        return;
    }
    QStringList parts;
    if (const auto q = services->quota(*p)) {
        parts << tr("%1 of %2 calls left today").arg(q->remaining).arg(q->limit);
    }
    if (p->requiresKey) {
        parts << (services->hasApiKey(p->key)
                      ? tr("API key set")
                      : tr("no API key (set %1)")
                            .arg(QString::fromStdString(DataServices::keyVariable(p->key))));
    }
    if (services->http().offline()) {
        parts << tr("offline: cached data only");
    }
    quota_->setText(parts.join(QStringLiteral(" · ")));
}

void TerrainPanel::addFiles() {
    const QStringList chosen = QFileDialog::getOpenFileNames(
        this, tr("Add Elevation Files"), QString(),
        tr("Elevation data (*.tif *.tiff *.hgt *.asc *.img *.las *.laz);;"
           "Elevation rasters (*.tif *.tiff *.hgt *.asc *.img);;"
           "Lidar point clouds (*.las *.laz);;All files (*)"));
    for (const QString& path : chosen) {
        const std::filesystem::path file(path.toStdString());
        const auto format = terrain::elevationFormatOf(file);
        const QString name = QString::fromStdString(file.filename().string());
        if (!format) {
            showStatus(tr("“%1” is not an elevation raster or a LAS/LAZ point cloud.").arg(name),
                       true);
            continue;
        }
        // One kind per terrain (FR-TER-F04).
        if (files_->count() > 0 &&
            (*format == terrain::ElevationFormat::PointCloud) != filesArePointClouds()) {
            showStatus(tr("“%1”: rasters and point clouds cannot be mixed.").arg(name), true);
            continue;
        }
        auto* item = new QListWidgetItem(QString::fromStdString(file.filename().string()));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        files_->addItem(item);
    }
    updateEnabled();
}

void TerrainPanel::addOrthoFiles() {
    const QStringList chosen = QFileDialog::getOpenFileNames(
        this, tr("Add Orthophotos"), QString(),
        tr("Orthophotos (*.tif *.tiff *.jp2 *.png *.jpg *.jpeg);;All files (*)"));
    for (const QString& path : chosen) {
        const std::filesystem::path file(path.toStdString());
        if (!terrain::isOrthophotoFile(file)) {
            showStatus(tr("“%1” is not a supported orthophoto (COG / GeoTIFF, JPEG 2000, PNG or "
                          "JPEG).")
                           .arg(QString::fromStdString(file.filename().string())),
                       true);
            continue;
        }
        auto* item = new QListWidgetItem(QString::fromStdString(file.filename().string()));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        orthoFiles_->addItem(item);
    }
    updateEnabled();
}

void TerrainPanel::fillImageryProviders() {
    const QString current = orthoSource_->currentData().toString();
    const QSignalBlocker blocker(orthoSource_);
    orthoSource_->clear();
    orthoSource_->addItem(tr("Orthophoto files"), QString());
    if (DataServices* services = scene_->dataServices()) {
        for (const net::ProviderConfig* p : services->imageryProviders()) {
            orthoSource_->addItem(QString::fromStdString(p->name), QString::fromStdString(p->key));
        }
    }
    orthoSource_->setCurrentIndex(std::max(orthoSource_->findData(current), 0));
    fillImageryLayers();
}

void TerrainPanel::fillImageryLayers() {
    const QString current = orthoLayer_->currentData().toString();
    orthoLayer_->clear();
    DataServices* services = scene_->dataServices();
    const net::ProviderConfig* p =
        services != nullptr && orthoSource_->currentIndex() > 0
            ? services->provider(orthoSource_->currentData().toString().toStdString())
            : nullptr;
    if (p == nullptr) {
        return;
    }
    for (const net::DatasetConfig& d : p->datasets) {
        orthoLayer_->addItem(QString::fromStdString(d.name), QString::fromStdString(d.id));
    }
    const int i = orthoLayer_->findData(current);
    orthoLayer_->setCurrentIndex(
        i >= 0 ? i : std::max(orthoLayer_->findData(QString::fromStdString(p->defaultDataset)), 0));
}

void TerrainPanel::updateOrthoInfo() {
    // FR-TER-I10: coverage and resolution of the imagery in use.
    QStringList lines;
    if (const auto* online =
            dynamic_cast<const terrain::OnlineImagerySource*>(scene_->orthophotos().get())) {
        // FR-TER-O12 for imagery: provider, protocol, layer, resolution, what was loaded.
        const terrain::OrthoMetadata& m = online->metadata();
        const terrain::OnlineImageryStatus st = online->status();
        lines << tr("%1 · %2 (%3), zoom %4 … %5, finest %6 m/px")
                     .arg(QString::fromStdString(online->provider().name))
                     .arg(QString::fromStdString(online->dataset().name))
                     .arg(QString::fromStdString(m.format))
                     .arg(online->dataset().minZoom)
                     .arg(online->dataset().maxZoom.value_or(net::kMaxImageryZoom))
                     .arg(m.resolutionM, 0, 'g', 2);
        lines << tr("Tiles: %1 downloaded, %2 from the cache, %3 loading, %4 not available")
                     .arg(st.downloaded)
                     .arg(st.fromCache)
                     .arg(st.pending)
                     .arg(st.missing);
        if (!st.lastError.empty()) {
            lines << tr("Last problem: %1").arg(QString::fromStdString(st.lastError));
        }
    } else if (const auto& ortho = scene_->orthophotos()) {
        const terrain::OrthoMetadata& m = ortho->metadata();
        lines << tr("%1, %2 file(s), %3 × %4 px, %5 m/px, %6, %7")
                     .arg(QString::fromStdString(m.format))
                     .arg(m.fileCount)
                     .arg(m.widthPx)
                     .arg(m.heightPx)
                     .arg(m.resolutionM, 0, 'g', 3)
                     .arg(QString::fromStdString(m.bands))
                     .arg(QString::fromStdString(m.crsCode.empty() ? m.crs : m.crsCode));
        lines << tr("Covers %1 × %2 km (x %3 … %4 km, z %5 … %6 km from the origin)")
                     .arg((m.maxX - m.minX) / 1000.0, 0, 'f', 2)
                     .arg((m.maxZ - m.minZ) / 1000.0, 0, 'f', 2)
                     .arg(m.minX / 1000.0, 0, 'f', 2)
                     .arg(m.maxX / 1000.0, 0, 'f', 2)
                     .arg(m.minZ / 1000.0, 0, 'f', 2)
                     .arg(m.maxZ / 1000.0, 0, 'f', 2);
        for (const std::string& note : m.notes) {
            lines << QString::fromStdString(note);
        }
    }
    if (!scene_->orthoWarning().empty()) {
        lines << QString::fromStdString(scene_->orthoWarning());
    }
    orthoInfo_->setText(lines.join(QLatin1Char('\n')));
}

void TerrainPanel::updateMetadata() {
    updateOrthoInfo();
    const auto& meta = scene_->terrainMetadata();
    if (!meta) {
        metadata_->clear();
        return;
    }
    QStringList lines;
    if (const auto& online = scene_->onlineMetadata()) {
        // FR-TER-O12: provider, protocol, CRS, coverage, resolution, cached area, heights.
        lines << tr("Provider: %1 (%2) · %3")
                     .arg(QString::fromStdString(online->providerName))
                     .arg(QString::fromStdString(online->protocol))
                     .arg(QString::fromStdString(online->datasetName));
        lines << tr("Resolution: %1 m · %2%3 · CRS %4")
                     .arg(online->resolutionM, 0, 'g', 3)
                     .arg(online->verticalDatum.empty()
                              ? tr("datum unknown")
                              : QString::fromStdString(online->verticalDatum))
                     .arg(online->ellipsoidal ? tr(" (ellipsoidal)") : QString())
                     .arg(
                         QString::fromStdString(meta->crsCode.empty() ? meta->crs : meta->crsCode));
        lines << (online->coverage ? tr("Coverage: %1° … %2° N, %3° … %4° E")
                                         .arg(online->coverage->south)
                                         .arg(online->coverage->north)
                                         .arg(online->coverage->west)
                                         .arg(online->coverage->east)
                                   : tr("Coverage: worldwide"));
        if (const auto status = scene_->onlineStatus()) {
            lines
                << tr("Streaming %1 km blocks: %2 loaded, %3 loading (%4 MB) · heights %5 … %6 m")
                       .arg(scene_->dataServices() != nullptr &&
                                    scene_->dataServices()->provider(online->providerKey)
                                ? scene_->dataServices()->provider(online->providerKey)->blockSizeKm
                                : 0.0)
                       .arg(status->loadedBlocks)
                       .arg(status->pendingBlocks)
                       .arg(static_cast<double>(status->bytes) / 1.0e6, 0, 'f', 1)
                       .arg(status->minHeightM, 0, 'f', 1)
                       .arg(status->maxHeightM, 0, 'f', 1);
            if (!status->lastError.empty()) {
                lines << tr("Last problem: %1").arg(QString::fromStdString(status->lastError));
            }
        } else {
            lines << tr("Cached area: %1 × %2 km in %3 request(s), %4 MB · heights %5 … %6 m")
                         .arg(online->area.widthKm(), 0, 'f', 1)
                         .arg(online->area.heightKm(), 0, 'f', 1)
                         .arg(online->pieces)
                         .arg(static_cast<double>(online->bytes) / 1.0e6, 0, 'f', 1)
                         .arg(meta->minHeightM, 0, 'f', 1)
                         .arg(meta->maxHeightM, 0, 'f', 1);
            lines << tr("Terrain: %1 × %2 km")
                         .arg(meta->extent.widthM / 1000.0, 0, 'f', 2)
                         .arg(meta->extent.lengthM / 1000.0, 0, 'f', 2);
        }
        lines << tr("Attribution: %1").arg(QString::fromStdString(online->attribution));
        metadata_->setText(lines.join(QLatin1Char('\n')));
        return;
    }
    lines << tr("Format: %1, %2 file(s), %3 × %4 px%5")
                 .arg(QString::fromStdString(meta->format))
                 .arg(meta->fileCount)
                 .arg(meta->widthPx)
                 .arg(meta->heightPx)
                 .arg(meta->hasOverviews ? tr(", with overviews") : QString());
    lines << tr("CRS: %1").arg(QString::fromStdString(
        meta->crsCode.empty() ? meta->crs : meta->crsCode + " — " + meta->crs));
    lines << tr("Resolution: %1 m · heights %2 … %3 m (%4)")
                 .arg(meta->resolutionM, 0, 'g', 3)
                 .arg(meta->minHeightM, 0, 'f', 1)
                 .arg(meta->maxHeightM, 0, 'f', 1)
                 .arg(meta->verticalDatum.empty() ? tr("datum unknown")
                                                  : QString::fromStdString(meta->verticalDatum));
    lines << tr("Terrain: %1 × %2 km")
                 .arg(meta->extent.widthM / 1000.0, 0, 'f', 2)
                 .arg(meta->extent.lengthM / 1000.0, 0, 'f', 2);
    if (meta->noDataPercent > 0.0) {
        lines << tr("NoData: %1 % (filled)").arg(meta->noDataPercent, 0, 'f', 1);
    }
    for (const std::string& note : meta->notes) {
        lines << QString::fromStdString(note);
    }
    metadata_->setText(lines.join(QLatin1Char('\n')));
}

void TerrainPanel::updateEnabled() {
    const bool custom = dimensions_->currentIndex() == 1;
    const bool file = source_->currentIndex() == 2;
    const bool online = source_->currentIndex() == 1;
    procedural_->setVisible(!file && !online);
    fileBox_->setVisible(file);
    onlineBox_->setVisible(online);
    orthoBox_->setVisible(file || online);
    removeOrtho_->setEnabled(!orthoFiles_->selectedItems().isEmpty());
    const bool files = orthoSource_->currentIndex() <= 0;
    orthoForm_->setRowVisible(orthoLayer_, !files);
    orthoForm_->setRowVisible(orthoFiles_, files);
    orthoForm_->setRowVisible(orthoButtons_, files);
    orthoForm_->setRowVisible(orthoCrs_, files);
    orthoForm_->setRowVisible(orthoBands_, files);
    orthoForm_->setRowVisible(orthoAttribution_, files);
    autoOrigin_->setVisible(file || online);
    autoOrigin_->setText(online ? tr("At ground level")
                                : tr("Centre of the data, at ground level"));
    const bool automatic = (file || online) && autoOrigin_->isChecked();
    latitude_->setEnabled(!(file && automatic));
    longitude_->setEnabled(!(file && automatic));
    altitude_->setEnabled(!automatic);
    removeFile_->setEnabled(!files_->selectedItems().isEmpty());
    const bool lidar = filesArePointClouds();
    lidarBox_->setVisible(lidar);
    band_->setVisible(!lidar);
    bandLabel_->setVisible(!lidar);
    lidarResolution_->setEnabled(!lidarAuto_->isChecked());
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
    preview_->setPixmap(QPixmap::fromImage(renderPreview(*scene_->source(), kPreviewSize)));
}

QImage TerrainPanel::renderPreview(const TerrainConfig& config, int size) {
    auto built = buildTerrain(config);
    if (!built) {
        QImage image(size, size, QImage::Format_RGB32);
        image.fill(Qt::darkGray);
        return image;
    }
    return renderPreview(*built->source, size);
}

QImage TerrainPanel::renderPreview(const terrain::HeightSource& source, int size) {
    QImage image(size, size, QImage::Format_RGB32);
    const auto extent = source.extent();
    const double extentM = extent ? std::max(extent->widthM, extent->lengthM) : kUnboundedPreviewM;
    const std::vector<float> heights =
        terrain::sampleHeightMap(source, 0.0, 0.0, extentM, size, size);
    const auto [lo, hi] = std::ranges::minmax_element(heights);
    const double minH = *lo;
    const double range = std::max(static_cast<double>(*hi) - minH, 1.0);
    const double cell = extentM / size;
    const std::optional<double> water = source.waterLevel();
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
