// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QImage>
#include <QWidget>

#include "ds/app/scene_config.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QGroupBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimer;

/// @file
/// @brief Terrain tab (SPECIFICATION §4.11): presets, source (procedural parameters,
/// elevation files, or an online provider with area download and quota), metadata,
/// dimensions, geographic origin, *Regenerate* and a 2D height preview.
/// @ingroup app

namespace ds::app {

class SceneController;

/// @brief Edits the scene's terrain. Changes are applied with *Regenerate*.
class TerrainPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Shows and edits the terrain of @p scene (which must outlive the panel).
    explicit TerrainPanel(SceneController& scene, QWidget* parent = nullptr);

    /// @brief The configuration currently entered in the form.
    [[nodiscard]] TerrainConfig formConfig() const;
    /// @brief Fills the form with @p config.
    void setFormConfig(const TerrainConfig& config);
    /// @brief Applies the form to the scene; shows the error, if any, in the panel.
    bool regenerate();
    /// @brief Downloads the online data of the form (FR-TER-O09) without applying it; shows
    /// the outcome in the panel. True if the data is cached afterwards.
    bool downloadArea();
    /// @brief Renders a shaded height map of @p config, @p size pixels square.
    [[nodiscard]] static QImage renderPreview(const TerrainConfig& config, int size);
    /// @brief Renders a shaded height map of @p source, @p size pixels square.
    [[nodiscard]] static QImage renderPreview(const terrain::HeightSource& source, int size);

private:
    void buildUi();
    void reloadPresets();
    void onPresetSelected(int index);
    void savePreset();
    void deletePreset();
    void updatePreview();
    void updateEnabled();
    void showStatus(const QString& text, bool error);
    void addFiles();
    void updateMetadata();
    void fillProviders();
    void fillDatasets();
    void updateQuota();
    // Downloads the online data of @p config if needed; false (with the status shown) if it
    // is not cached afterwards.
    bool ensureDownloaded(const TerrainConfig& config);
    // Makes the DTM of a point-cloud @p config if needed; false (with the status shown) if it
    // is not cached afterwards.
    bool ensureLidarDem(const TerrainConfig& config);
    // True if the listed files are LAS / LAZ.
    [[nodiscard]] bool filesArePointClouds() const;

    SceneController* scene_;
    QComboBox* presets_ = nullptr;
    QPushButton* deletePreset_ = nullptr;
    QComboBox* source_ = nullptr;
    QComboBox* dimensions_ = nullptr;
    QDoubleSpinBox* widthKm_ = nullptr;
    QDoubleSpinBox* lengthKm_ = nullptr;
    QLineEdit* seed_ = nullptr;
    QDoubleSpinBox* featureSize_ = nullptr;
    QDoubleSpinBox* amplitude_ = nullptr;
    QDoubleSpinBox* baseHeight_ = nullptr;
    QSpinBox* octaves_ = nullptr;
    QDoubleSpinBox* lacunarity_ = nullptr;
    QDoubleSpinBox* persistence_ = nullptr;
    QDoubleSpinBox* mountains_ = nullptr;
    QDoubleSpinBox* warp_ = nullptr;
    QDoubleSpinBox* erosion_ = nullptr;
    QCheckBox* water_ = nullptr;
    QDoubleSpinBox* waterLevel_ = nullptr;
    QComboBox* edge_ = nullptr;
    QDoubleSpinBox* latitude_ = nullptr;
    QDoubleSpinBox* longitude_ = nullptr;
    QDoubleSpinBox* altitude_ = nullptr;
    QCheckBox* autoOrigin_ = nullptr;
    QGroupBox* procedural_ = nullptr;
    QGroupBox* fileBox_ = nullptr;
    QListWidget* files_ = nullptr;
    QPushButton* removeFile_ = nullptr;
    QSpinBox* band_ = nullptr;
    QLineEdit* crs_ = nullptr;
    QLineEdit* datum_ = nullptr;
    QLineEdit* attribution_ = nullptr;
    QComboBox* fileEdge_ = nullptr;
    QWidget* lidarBox_ = nullptr;  // LAS / LAZ settings (FR-TER-L02, L03).
    QCheckBox* lidarAuto_ = nullptr;
    QDoubleSpinBox* lidarResolution_ = nullptr;
    QComboBox* lidarInterpolation_ = nullptr;
    QLabel* bandLabel_ = nullptr;
    QGroupBox* onlineBox_ = nullptr;
    QComboBox* provider_ = nullptr;
    QComboBox* dataset_ = nullptr;
    QComboBox* onlineEdge_ = nullptr;
    QPushButton* download_ = nullptr;
    QLabel* quota_ = nullptr;
    QTimer* refresh_ = nullptr;
    QLabel* metadata_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* preview_ = nullptr;
    terrain::ProceduralParams hidden_;  // Parameters without widgets (flat zones, snow line).
};

}  // namespace ds::app
