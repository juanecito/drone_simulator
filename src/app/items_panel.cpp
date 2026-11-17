// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/items_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

#include "ds/app/items_controller.hpp"
#include "ds/app/mission_dialogs.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/physics/frames.hpp"

namespace ds::app {
namespace {

using items::ItemType;

QDoubleSpinBox* spin(const char* name, double min, double max, int decimals, const char* suffix) {
    auto* s = new QDoubleSpinBox;
    s->setObjectName(QLatin1String(name));
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSuffix(QString::fromUtf8(suffix));
    s->setKeyboardTracking(false);
    return s;
}

QString typeText(ItemType type) {
    switch (type) {
        case ItemType::Gate: return ItemsPanel::tr("Gate");
        case ItemType::Waypoint: return ItemsPanel::tr("Waypoint");
        case ItemType::LandingPad: return ItemsPanel::tr("Landing pad");
        case ItemType::Obstacle: return ItemsPanel::tr("Obstacle");
        case ItemType::Payload: return ItemsPanel::tr("Payload");
        case ItemType::PickupZone: return ItemsPanel::tr("Pickup zone");
        case ItemType::DropZone: return ItemsPanel::tr("Drop zone");
        case ItemType::InspectionTarget: return ItemsPanel::tr("Inspection target");
        case ItemType::SurveyArea: return ItemsPanel::tr("Survey area");
        case ItemType::NoFlyZone: return ItemsPanel::tr("No-fly zone");
        case ItemType::Geofence: return ItemsPanel::tr("Geofence");
        case ItemType::PilotPosition: return ItemsPanel::tr("Pilot");
        default: return QString::fromUtf8(items::itemTypeName(type).data());
    }
}

std::string shapeOf(const items::Item& item) {
    const auto& p = item.properties;
    return p.contains("shape") && p["shape"].is_string() ? p["shape"].get<std::string>() : "box";
}

double number(const common::Json& p, const char* key, double fallback) {
    return p.contains(key) && p[key].is_number() ? p[key].get<double>() : fallback;
}

}  // namespace

ItemsPanel::ItemsPanel(ItemsController& items, SimulationController& simulation,
                       SceneController& scene, QWidget* parent)
    : QWidget(parent), items_(&items), simulation_(&simulation), scene_(&scene) {
    buildUi();
    connect(items_, &ItemsController::changed, this, &ItemsPanel::refresh);
    connect(simulation_, &SimulationController::missionActiveChanged, this, &ItemsPanel::refresh);
    refresh();
}

void ItemsPanel::buildUi() {
    note_ = new QLabel;
    note_->setObjectName(QStringLiteral("ItemsNote"));
    note_->setWordWrap(true);

    add_ = new QToolButton;
    add_->setObjectName(QStringLiteral("AddItemButton"));
    add_->setText(tr("Add"));
    add_->setPopupMode(QToolButton::InstantPopup);
    auto* palette = new QMenu(add_);
    for (int i = 0; i < static_cast<int>(itemPalette().size()); ++i) {
        palette->addAction(tr(itemPalette()[static_cast<std::size_t>(i)].label.c_str()), this,
                           [this, i] { addFromPalette(i); });
    }
    add_->setMenu(palette);
    const auto button = [this](const char* name, const QString& text) {
        auto* b = new QPushButton(text);
        b->setObjectName(QLatin1String(name));
        return b;
    };
    duplicate_ = button("DuplicateItem", tr("Duplicate"));
    delete_ = button("DeleteItem", tr("Delete"));
    snap_ = button("SnapItem", tr("Snap to terrain"));
    undo_ = button("UndoItems", tr("Undo"));
    redo_ = button("RedoItems", tr("Redo"));
    auto* tools = new QHBoxLayout;
    for (QWidget* w : {static_cast<QWidget*>(add_), static_cast<QWidget*>(duplicate_),
                       static_cast<QWidget*>(delete_), static_cast<QWidget*>(snap_)}) {
        tools->addWidget(w);
    }
    tools->addStretch(1);
    tools->addWidget(undo_);
    tools->addWidget(redo_);

    tree_ = new QTreeWidget;
    tree_->setObjectName(QStringLiteral("ItemTree"));
    tree_->setHeaderLabels({tr("Name"), tr("Type"), tr("Position")});
    tree_->setRootIsDecorated(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    // Property editor.
    editor_ = new QGroupBox(tr("Properties"));
    editor_->setObjectName(QStringLiteral("ItemProperties"));
    name_ = new QLineEdit;
    name_->setObjectName(QStringLiteral("ItemName"));
    east_ = spin("ItemEast", -1e6, 1e6, 1, " m");
    north_ = spin("ItemNorth", -1e6, 1e6, 1, " m");
    latitude_ = spin("ItemLatitude", -90.0, 90.0, 6, "°");
    longitude_ = spin("ItemLongitude", -180.0, 180.0, 6, "°");
    altitude_ = spin("ItemAltitude", -500.0, 9000.0, 1, " m");
    altRef_ = new QComboBox;
    altRef_->setObjectName(QStringLiteral("ItemAltRef"));
    altRef_->addItem(tr("above ground (AGL)"));
    altRef_->addItem(tr("above sea level (AMSL)"));
    heading_ = spin("ItemHeading", 0.0, 360.0, 0, "°");
    heading_->setWrapping(true);
    shape_ = new QComboBox;
    shape_->setObjectName(QStringLiteral("ItemShape"));
    for (const char* s : {"box", "building", "cylinder", "tree", "house", "cow", "man"}) {
        shape_->addItem(QLatin1String(s));
    }
    radius_ = spin("ItemRadius", 0.1, 5000.0, 1, " m");
    width_ = spin("ItemWidth", 0.1, 1000.0, 1, " m");
    length_ = spin("ItemLength", 0.1, 1000.0, 1, " m");
    height_ = spin("ItemHeight", 0.1, 1000.0, 1, " m");
    mass_ = spin("ItemMass", 0.1, 1e9, 1, " kg");
    mass_->setToolTip(
        tr("Obstacles stay in place; a light one (a person, a cow) takes part of "
           "the impact when a heavy drone hits it."));
    enabled_ = new QCheckBox(tr("Enabled"));
    enabled_->setObjectName(QStringLiteral("ItemEnabled"));
    apply_ = button("ApplyItem", tr("Apply"));
    auto* altitude = new QHBoxLayout;
    altitude->addWidget(altitude_, 1);
    altitude->addWidget(altRef_, 1);
    auto* form = new QFormLayout(editor_);
    form->addRow(tr("Name"), name_);
    form->addRow(tr("East"), east_);
    form->addRow(tr("North"), north_);
    form->addRow(tr("Latitude"), latitude_);
    form->addRow(tr("Longitude"), longitude_);
    form->addRow(tr("Altitude"), altitude);
    form->addRow(tr("Heading"), heading_);
    form->addRow(tr("Shape"), shape_);
    form->addRow(tr("Radius"), radius_);
    form->addRow(tr("Width"), width_);
    form->addRow(tr("Length"), length_);
    form->addRow(tr("Height"), height_);
    form->addRow(tr("Mass"), mass_);
    form->addRow(QString(), enabled_);
    form->addRow(QString(), apply_);

    // Gate order.
    gates_ = new QGroupBox(tr("Gate order"));
    gates_->setObjectName(QStringLiteral("GateOrderGroup"));
    gateList_ = new QListWidget;
    gateList_->setObjectName(QStringLiteral("GateOrder"));
    gateUp_ = button("GateUp", tr("Earlier"));
    gateDown_ = button("GateDown", tr("Later"));
    auto* gateButtons = new QVBoxLayout;
    gateButtons->addWidget(gateUp_);
    gateButtons->addWidget(gateDown_);
    gateButtons->addStretch(1);
    auto* gateLayout = new QHBoxLayout(gates_);
    gateLayout->addWidget(gateList_, 1);
    gateLayout->addLayout(gateButtons);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->addWidget(note_);
    layout->addLayout(tools);
    layout->addWidget(tree_, 2);
    layout->addWidget(editor_);
    layout->addWidget(gates_, 1);
    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] {
        loadEditor();
        updateButtons();
    });
    connect(duplicate_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedItem()) {
            if (auto copy = items_->duplicate(*id)) {
                selectItem(*copy);
            }
        }
    });
    connect(delete_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedItem()) {
            items_->remove(*id);
        }
    });
    connect(snap_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedItem()) {
            items_->snapToTerrain(*id);
        }
    });
    connect(undo_, &QPushButton::clicked, items_, &ItemsController::undo);
    connect(redo_, &QPushButton::clicked, items_, &ItemsController::redo);
    connect(apply_, &QPushButton::clicked, this, &ItemsPanel::applyEdits);
    connect(shape_, &QComboBox::currentIndexChanged, this, &ItemsPanel::shapeChosen);
    connect(latitude_, &QDoubleSpinBox::editingFinished, this, &ItemsPanel::latLonEdited);
    connect(longitude_, &QDoubleSpinBox::editingFinished, this, &ItemsPanel::latLonEdited);
    connect(east_, &QDoubleSpinBox::editingFinished, this, &ItemsPanel::eastNorthEdited);
    connect(north_, &QDoubleSpinBox::editingFinished, this, &ItemsPanel::eastNorthEdited);
    const auto moveGate = [this](int delta) {
        const int row = gateList_->currentRow();
        const auto order = items_->gateOrder();
        if (row < 0 || row >= static_cast<int>(order.size())) {
            return;
        }
        if (items_->moveGate(order[static_cast<std::size_t>(row)], delta)) {
            gateList_->setCurrentRow(row + delta);
        }
    };
    connect(gateUp_, &QPushButton::clicked, this, [moveGate] { moveGate(-1); });
    connect(gateDown_, &QPushButton::clicked, this, [moveGate] { moveGate(1); });
    connect(gateList_, &QListWidget::currentRowChanged, this, [this] { updateButtons(); });
}

void ItemsPanel::refresh() {
    const bool mission = simulation_->missionActive();
    const std::optional<items::ItemId> keep = selectedItem();
    shown_.clear();
    if (mission) {
        if (auto map = missionMapData(*simulation_->mission())) {
            shown_ = std::move(map->items);
        }
        note_->setText(
            tr("Items of the mission (read only). Your own items come back after "
               "the mission."));
    } else {
        shown_ = items_->items();
        note_->setText(shown_.empty() ? tr("No items yet: use Add to place gates, obstacles, "
                                           "pads or zones in front of the drone.")
                                      : tr("Your items of the free-flight scene."));
    }
    loading_ = true;
    tree_->clear();
    for (std::size_t i = 0; i < shown_.size(); ++i) {
        const items::Item& item = shown_[i];
        const items::Vec3& p = item.transform.position;
        auto* row = new QTreeWidgetItem(
            tree_,
            {QString::fromStdString(item.name.empty() ? item.key : item.name), typeText(item.type),
             tr("%1 E, %2 N, %3 m %4")
                 .arg(p.x, 0, 'f', 0)
                 .arg(-p.z, 0, 'f', 0)
                 .arg(item.altitudeM, 0, 'f', 0)
                 .arg(item.altitudeRef == items::AltitudeRef::Amsl ? tr("AMSL") : tr("AGL"))});
        row->setData(0, Qt::UserRole, static_cast<qulonglong>(i));
        if (!item.enabled) {
            row->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));
        }
    }
    loading_ = false;
    if (keep && !mission) {
        selectItem(*keep);
    }
    if (tree_->currentItem() == nullptr && tree_->topLevelItemCount() > 0) {
        tree_->setCurrentItem(tree_->topLevelItem(0));
    }
    loadEditor();
    refreshGateOrder();
    updateButtons();
}

void ItemsPanel::refreshGateOrder() {
    const int row = gateList_->currentRow();
    gateList_->clear();
    if (simulation_->missionActive()) {
        gates_->setVisible(false);
        return;
    }
    const auto order = items_->gateOrder();
    gates_->setVisible(!order.empty());
    int n = 1;
    for (const items::ItemId id : order) {
        const items::Item* g = items_->registry().get(id);
        gateList_->addItem(QStringLiteral("%1. %2").arg(n++).arg(QString::fromStdString(g->name)));
    }
    gateList_->setCurrentRow(std::min(row, gateList_->count() - 1));
}

bool ItemsPanel::selectItem(items::ItemId id) {
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* row = tree_->topLevelItem(i);
        const auto index = row->data(0, Qt::UserRole).toULongLong();
        if (index < shown_.size() && shown_[index].id == id && !simulation_->missionActive()) {
            tree_->setCurrentItem(row);
            return true;
        }
    }
    return false;
}

std::optional<items::ItemId> ItemsPanel::selectedItem() const {
    const QTreeWidgetItem* row = tree_->currentItem();
    if (row == nullptr || simulation_->missionActive()) {
        return std::nullopt;
    }
    const auto index = row->data(0, Qt::UserRole).toULongLong();
    return index < shown_.size() ? std::optional(shown_[index].id) : std::nullopt;
}

void ItemsPanel::loadEditor() {
    const QTreeWidgetItem* row = tree_->currentItem();
    const auto index = row != nullptr ? row->data(0, Qt::UserRole).toULongLong() : shown_.size();
    editor_->setEnabled(index < shown_.size());
    if (index >= shown_.size()) {
        return;
    }
    const items::Item& item = shown_[index];
    loading_ = true;
    name_->setText(QString::fromStdString(item.name));
    east_->setValue(item.transform.position.x);
    north_->setValue(-item.transform.position.z);
    altitude_->setValue(item.altitudeM);
    altRef_->setCurrentIndex(item.altitudeRef == items::AltitudeRef::Amsl ? 1 : 0);
    heading_->setValue(std::fmod(items::headingOf(item.transform.rotation) + 360.0, 360.0));
    const auto& p = item.properties;
    shape_->setCurrentText(QString::fromStdString(shapeOf(item)));
    radius_->setValue(number(p, "radiusM", item.shape.radiusM > 0.0 ? item.shape.radiusM : 1.0));
    if (item.type == ItemType::Gate) {
        width_->setValue(number(p, "widthM", 2.0));
        height_->setValue(number(p, "heightM", 2.0));
    } else if (p.contains("sizeM") && p["sizeM"].is_array() && p["sizeM"].size() == 3) {
        width_->setValue(p["sizeM"][0].get<double>());
        height_->setValue(p["sizeM"][1].get<double>());
        length_->setValue(p["sizeM"][2].get<double>());
    } else {
        height_->setValue(number(p, "heightM", 8.0));
    }
    loadedMassKg_ = items::massKg(item);
    mass_->setValue(std::isfinite(loadedMassKg_) ? loadedMassKg_ : 0.0);
    loadedMassKg_ = mass_->value();  // As rounded by the spin box.
    enabled_->setChecked(item.enabled);
    loading_ = false;
    eastNorthEdited();
    updateFieldVisibility();
    // Mission items are shown, not edited.
    const bool editable = !simulation_->missionActive();
    for (QWidget* w : editor_->findChildren<QWidget*>()) {
        if (w != editor_) {
            w->setEnabled(editable);
        }
    }
}

void ItemsPanel::updateFieldVisibility() {
    const QTreeWidgetItem* row = tree_->currentItem();
    const auto index = row != nullptr ? row->data(0, Qt::UserRole).toULongLong() : shown_.size();
    if (index >= shown_.size()) {
        return;
    }
    const ItemType type = shown_[index].type;
    const QString shape = shape_->currentText();
    const bool obstacle = type == ItemType::Obstacle;
    const bool round =
        obstacle && (shape == QLatin1String("cylinder") || shape == QLatin1String("tree"));
    const bool person = obstacle && shape == QLatin1String("man");  // Sized by height only.
    const bool boxy = obstacle && !round && !person;
    const bool hasRadius = round || type == ItemType::Waypoint || type == ItemType::LandingPad ||
                           type == ItemType::PickupZone || type == ItemType::DropZone ||
                           type == ItemType::NoFlyZone || type == ItemType::Geofence ||
                           type == ItemType::InspectionTarget;
    auto* form = static_cast<QFormLayout*>(editor_->layout());
    form->setRowVisible(shape_, obstacle);
    form->setRowVisible(radius_, hasRadius);
    form->setRowVisible(width_, boxy || type == ItemType::Gate);
    form->setRowVisible(length_, boxy);
    form->setRowVisible(height_, obstacle || type == ItemType::Gate ||
                                     type == ItemType::PickupZone || type == ItemType::DropZone);
    form->setRowVisible(mass_, obstacle);
}

void ItemsPanel::shapeChosen() {
    if (!loading_) {
        // A new figure starts at its typical size and mass.
        const common::Json p =
            defaultProperties({ItemType::Obstacle, shape_->currentText().toStdString(), ""});
        if (p.contains("radiusM")) {
            radius_->setValue(p["radiusM"].get<double>());
        }
        if (p.contains("sizeM") && p["sizeM"].size() == 3) {
            width_->setValue(p["sizeM"][0].get<double>());
            height_->setValue(p["sizeM"][1].get<double>());
            length_->setValue(p["sizeM"][2].get<double>());
        } else if (p.contains("heightM")) {
            height_->setValue(p["heightM"].get<double>());
        }
        const items::Shape shape = items::defaultShape(ItemType::Obstacle, p);
        mass_->setValue(items::typicalMassKg(ItemType::Obstacle, p, shape));
        loadedMassKg_ = mass_->value();
    }
    updateFieldVisibility();
}

void ItemsPanel::eastNorthEdited() {
    if (loading_) {
        return;
    }
    const geo::Geodetic g = scene_->toGeodetic({east_->value(), 0.0, -north_->value()});
    const QSignalBlocker a(latitude_);
    const QSignalBlocker b(longitude_);
    latitude_->setValue(g.latitudeDeg);
    longitude_->setValue(g.longitudeDeg);
}

void ItemsPanel::latLonEdited() {
    if (loading_) {
        return;
    }
    const geo::Enu enu = scene_->frame().toEnu(
        geo::Geodetic{latitude_->value(), longitude_->value(), scene_->frame().origin().heightM});
    const QSignalBlocker a(east_);
    const QSignalBlocker b(north_);
    const geo::WorldPosition w = geo::toWorld(enu);
    east_->setValue(w.x);
    north_->setValue(-w.z);
}

void ItemsPanel::applyEdits() {
    const auto id = selectedItem();
    const items::Item* current = id ? items_->registry().get(*id) : nullptr;
    if (current == nullptr) {
        return;
    }
    items::Item item = *current;
    item.name = name_->text().trimmed().toStdString();
    item.transform.position.x = east_->value();
    item.transform.position.z = -north_->value();
    item.altitudeM = altitude_->value();
    item.altitudeRef =
        altRef_->currentIndex() == 1 ? items::AltitudeRef::Amsl : items::AltitudeRef::Agl;
    item.transform.rotation = items::headingRotation(heading_->value());
    item.enabled = enabled_->isChecked();
    auto& p = item.properties;
    const auto* form = static_cast<QFormLayout*>(editor_->layout());
    if (form->isRowVisible(shape_)) {
        p["shape"] = shape_->currentText().toStdString();
    }
    if (form->isRowVisible(radius_)) {
        p["radiusM"] = radius_->value();
    }
    if (item.type == ItemType::Gate) {
        p["widthM"] = width_->value();
        p["heightM"] = height_->value();
    } else if (form->isRowVisible(length_)) {
        p["sizeM"] = common::Json::array({width_->value(), height_->value(), length_->value()});
        p.erase("heightM");
        p.erase("radiusM");
    } else if (form->isRowVisible(height_)) {
        p["heightM"] = height_->value();
        p.erase("sizeM");
    }
    // The mass is stored only once set by hand; otherwise it follows the size and shape.
    if (form->isRowVisible(mass_)) {
        if (mass_->value() != loadedMassKg_) {
            p["massKg"] = mass_->value();
        } else if (shapeOf(item) != shapeOf(*current)) {
            p.erase("massKg");
        }
    }
    if (auto updated = items_->update(std::move(item)); !updated) {
        QMessageBox::warning(this, tr("Items"), QString::fromStdString(updated.error().message));
    }
}

void ItemsPanel::addFromPalette(int index) {
    if (index < 0 || index >= static_cast<int>(itemPalette().size()) ||
        simulation_->missionActive()) {
        return;
    }
    // 15 m in front of the drone, facing it.
    const physics::RigidBodyState& b = simulation_->snapshot().body;
    const double yaw = physics::eulerFromOrientation(b.orientation).yaw;
    const double x = b.position.x + 15.0 * std::sin(yaw);
    const double z = b.position.z - 15.0 * std::cos(yaw);
    const double headingDeg = std::fmod(yaw * 180.0 / 3.14159265358979323846 + 360.0, 360.0);
    if (auto id = items_->add(itemPalette()[static_cast<std::size_t>(index)], x, z, headingDeg)) {
        selectItem(*id);
    }
}

void ItemsPanel::updateButtons() {
    const bool mission = simulation_->missionActive();
    const bool selected = selectedItem().has_value();
    add_->setEnabled(!mission);
    duplicate_->setEnabled(selected);
    delete_->setEnabled(selected);
    snap_->setEnabled(selected);
    undo_->setEnabled(!mission && items_->registry().canUndo());
    redo_->setEnabled(!mission && items_->registry().canRedo());
    const int row = gateList_->currentRow();
    gateUp_->setEnabled(row > 0);
    gateDown_->setEnabled(row >= 0 && row + 1 < gateList_->count());
}

}  // namespace ds::app
