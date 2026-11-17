// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>

#include <optional>
#include <vector>

#include "ds/items/item.hpp"

/// @file
/// @brief Items tab (§5.3): item list, add-item palette, property editor (name, position as
/// east/north or latitude/longitude, altitude AGL/AMSL, heading, type properties),
/// snap-to-terrain, duplicate/delete, undo/redo and the gate order. During a mission it lists
/// the mission's items, read only.
/// @ingroup app

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;
class QTreeWidget;

namespace ds::app {

class ItemsController;
class SceneController;
class SimulationController;

/// @brief The Items tab.
class ItemsPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Edits @p items; @p simulation places new items in front of the drone and tells
    /// when a mission is flown; @p scene converts positions. All must outlive the panel.
    ItemsPanel(ItemsController& items, SimulationController& simulation, SceneController& scene,
               QWidget* parent = nullptr);

    /// @brief Rebuilds the list (and the editor of the selected item).
    void refresh();
    /// @brief Adds an item of palette entry @p index in front of the drone and selects it.
    void addFromPalette(int index);
    /// @brief Selects item @p id (free-flight items).
    bool selectItem(items::ItemId id);
    /// @brief The selected free-flight item, if any.
    [[nodiscard]] std::optional<items::ItemId> selectedItem() const;
    /// @brief Writes the editor's values to the selected item.
    void applyEdits();

private:
    void buildUi();
    void loadEditor();
    void updateFieldVisibility();
    void shapeChosen();
    void updateButtons();
    void latLonEdited();
    void eastNorthEdited();
    void refreshGateOrder();

    ItemsController* items_;
    SimulationController* simulation_;
    SceneController* scene_;
    bool loading_ = false;
    double loadedMassKg_ = 0.0;       // Mass shown when the item was loaded (typical or set).
    std::vector<items::Item> shown_;  // Rows of the list (mission or free items).

    QLabel* note_ = nullptr;
    QToolButton* add_ = nullptr;
    QPushButton* duplicate_ = nullptr;
    QPushButton* delete_ = nullptr;
    QPushButton* snap_ = nullptr;
    QPushButton* undo_ = nullptr;
    QPushButton* redo_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QGroupBox* editor_ = nullptr;
    QLineEdit* name_ = nullptr;
    QDoubleSpinBox* east_ = nullptr;
    QDoubleSpinBox* north_ = nullptr;
    QDoubleSpinBox* latitude_ = nullptr;
    QDoubleSpinBox* longitude_ = nullptr;
    QDoubleSpinBox* altitude_ = nullptr;
    QComboBox* altRef_ = nullptr;
    QDoubleSpinBox* heading_ = nullptr;
    QComboBox* shape_ = nullptr;
    QDoubleSpinBox* radius_ = nullptr;
    QDoubleSpinBox* width_ = nullptr;
    QDoubleSpinBox* length_ = nullptr;
    QDoubleSpinBox* height_ = nullptr;
    QDoubleSpinBox* mass_ = nullptr;
    QCheckBox* enabled_ = nullptr;
    QPushButton* apply_ = nullptr;
    QGroupBox* gates_ = nullptr;
    QListWidget* gateList_ = nullptr;
    QPushButton* gateUp_ = nullptr;
    QPushButton* gateDown_ = nullptr;
};

}  // namespace ds::app
