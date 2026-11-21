// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/data_providers_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ds/app/data_services.hpp"

namespace ds::app {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

constexpr double kGb = 1024.0 * 1024.0 * 1024.0;

void disableItem(QComboBox* combo, int index, const QString& why) {
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    if (model != nullptr) {
        if (QStandardItem* item = model->item(index)) {
            item->setEnabled(false);
            item->setToolTip(why);
        }
    }
}

QLabel* wrapped(const QString& text = {}) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    return label;
}

}  // namespace

QString formatBytes(std::uint64_t bytes) {
    const auto b = static_cast<double>(bytes);
    if (b >= 1.0e9) {
        return QStringLiteral("%1 GB").arg(b / 1.0e9, 0, 'f', 2);
    }
    if (b >= 1.0e6) {
        return QStringLiteral("%1 MB").arg(b / 1.0e6, 0, 'f', 1);
    }
    if (b >= 1.0e3) {
        return QStringLiteral("%1 kB").arg(b / 1.0e3, 0, 'f', 0);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

// --- ProviderDialog ---------------------------------------------------------------------------

ProviderDialog::ProviderDialog(net::ProviderConfig start, bool isNew, QWidget* parent)
    : QDialog(parent), start_(std::move(start)) {
    setWindowTitle(isNew ? tr("Add Data Provider") : tr("Edit Data Provider"));
    auto* form = new QFormLayout;
    form_ = form;

    key_ = new QLineEdit(qs(start_.key));
    key_->setObjectName(QStringLiteral("ProviderKeyEdit"));
    key_->setReadOnly(!isNew);
    key_->setToolTip(
        tr("Stable identifier used by missions (\"elevationProvider\", \"imageryProvider\")."));
    form->addRow(tr("&Key:"), key_);
    name_ = new QLineEdit(qs(start_.name));
    name_->setObjectName(QStringLiteral("ProviderNameEdit"));
    form->addRow(tr("&Name:"), name_);
    kind_ = new QComboBox;
    kind_->setObjectName(QStringLiteral("ProviderKindCombo"));
    kind_->addItem(tr("Elevation"), QStringLiteral("elevation"));
    kind_->addItem(tr("Imagery"), QStringLiteral("imagery"));
    kind_->setCurrentIndex(start_.isImagery() ? 1 : 0);
    form->addRow(tr("Kind:"), kind_);
    protocol_ = new QComboBox;
    protocol_->setObjectName(QStringLiteral("ProtocolCombo"));
    form->addRow(tr("&Protocol:"), protocol_);
    url_ = new QLineEdit(qs(start_.urlTemplate));
    url_->setObjectName(QStringLiteral("UrlTemplateEdit"));
    form->addRow(tr("&URL template:"), url_);
    format_ = new QLineEdit(qs(start_.format.empty() ? std::string("GTiff") : start_.format));
    format_->setObjectName(QStringLiteral("FormatEdit"));
    form->addRow(tr("Format:"), format_);
    crs_ = new QLabel;
    form->addRow(tr("CRS:"), crs_);
    tileSize_ = new QSpinBox;
    tileSize_->setObjectName(QStringLiteral("TileSizeSpin"));
    tileSize_->setRange(256, 512);
    tileSize_->setSingleStep(256);
    tileSize_->setSuffix(QStringLiteral(" px"));
    tileSize_->setValue(start_.tileSizePx);
    form->addRow(tr("Tile size:"), tileSize_);

    datasets_ = new QTableWidget(0, 4);
    datasets_->setObjectName(QStringLiteral("DatasetsTable"));
    datasets_->horizontalHeader()->setStretchLastSection(true);
    datasets_->verticalHeader()->hide();
    datasets_->setMinimumHeight(110);
    for (const net::DatasetConfig& d : start_.datasets) {
        addDataset(d);
    }
    auto* addRow = new QPushButton(tr("Add Dataset"));
    addRow->setObjectName(QStringLiteral("AddDatasetButton"));
    auto* removeRow = new QPushButton(tr("Remove Dataset"));
    connect(addRow, &QPushButton::clicked, this, [this] { addDataset({}); });
    connect(removeRow, &QPushButton::clicked, this, [this] {
        if (datasets_->currentRow() >= 0) {
            datasets_->removeRow(datasets_->currentRow());
        }
    });
    auto* rowButtons = new QHBoxLayout;
    rowButtons->addWidget(addRow);
    rowButtons->addWidget(removeRow);
    rowButtons->addStretch();
    auto* datasetBox = new QVBoxLayout;
    datasetBox->addWidget(datasets_);
    datasetBox->addLayout(rowButtons);
    form->addRow(tr("Datasets:"), datasetBox);

    requiresKey_ = new QCheckBox(tr("Requests need an API key"));
    requiresKey_->setObjectName(QStringLiteral("RequiresKeyCheck"));
    requiresKey_->setChecked(start_.requiresKey);
    form->addRow(QString(), requiresKey_);
    keyPlace_ = new QComboBox;
    keyPlace_->setObjectName(QStringLiteral("KeyPlacementCombo"));
    keyPlace_->addItem(tr("Query parameter"), int(net::ApiKeyPlacement::Location::Query));
    keyPlace_->addItem(tr("HTTP header"), int(net::ApiKeyPlacement::Location::Header));
    keyPlace_->setCurrentIndex(start_.apiKey.location == net::ApiKeyPlacement::Location::Header);
    keyName_ = new QLineEdit(qs(start_.apiKey.name));
    keyName_->setObjectName(QStringLiteral("KeyNameEdit"));
    keyName_->setPlaceholderText(QStringLiteral("API_Key"));
    auto* keyRow = new QHBoxLayout;
    keyRow->addWidget(keyPlace_);
    keyRow->addWidget(keyName_, 1);
    form->addRow(tr("Key goes in:"), keyRow);

    dailyLimit_ = new QSpinBox;
    dailyLimit_->setObjectName(QStringLiteral("DailyLimitSpin"));
    dailyLimit_->setRange(0, 1'000'000);
    dailyLimit_->setSpecialValueText(tr("No limit"));
    dailyLimit_->setValue(start_.dailyCallLimit.value_or(0));
    form->addRow(tr("Calls per 24 h:"), dailyLimit_);
    concurrent_ = new QSpinBox;
    concurrent_->setRange(1, 16);
    concurrent_->setValue(start_.maxConcurrent);
    form->addRow(tr("Simultaneous requests:"), concurrent_);
    blockSize_ = new QDoubleSpinBox;
    blockSize_->setObjectName(QStringLiteral("BlockSizeSpin"));
    blockSize_->setRange(1.0, 200.0);
    blockSize_->setSuffix(QStringLiteral(" km"));
    blockSize_->setValue(start_.blockSizeKm);
    blockSize_->setToolTip(tr("Size of the blocks fetched ahead of the drone without an extent."));
    form->addRow(tr("Block size:"), blockSize_);
    allowCache_ = new QCheckBox(tr("The provider allows caching"));
    allowCache_->setChecked(start_.allowCache);
    form->addRow(QString(), allowCache_);
    insecure_ = new QCheckBox(tr("Allow plain HTTP (insecure)"));
    insecure_->setObjectName(QStringLiteral("InsecureHttpCheck"));
    insecure_->setChecked(start_.insecureHttp);
    insecure_->setToolTip(
        tr("Requests and the API key travel unencrypted. Use only for a server you control."));
    form->addRow(QString(), insecure_);
    attribution_ = new QLineEdit(qs(start_.attribution));
    attribution_->setObjectName(QStringLiteral("AttributionEdit"));
    form->addRow(tr("&Attribution:"), attribution_);
    licence_ = new QLineEdit(qs(start_.licence));
    form->addRow(tr("Licence:"), licence_);
    terms_ = new QLineEdit(qs(start_.termsUrl));
    form->addRow(tr("Terms URL:"), terms_);

    error_ = new QLabel;
    error_->setObjectName(QStringLiteral("ProviderErrorLabel"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #c0392b;"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProviderDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(requiresKey_, &QCheckBox::toggled, this, &ProviderDialog::updateEnabled);
    connect(kind_, &QComboBox::currentIndexChanged, this, &ProviderDialog::kindChanged);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(error_);
    layout->addWidget(buttons);
    kindChanged();
    if (const int i = protocol_->findData(qs(start_.protocol)); i >= 0) {
        protocol_->setCurrentIndex(i);
    }
    updateEnabled();
}

bool ProviderDialog::imagery() const { return kind_->currentIndex() == 1; }

void ProviderDialog::kindChanged() {
    const bool tiles = imagery();
    const QString protocol = protocol_->currentData().toString();
    protocol_->clear();
    if (tiles) {
        protocol_->addItem(tr("XYZ tiles ({z}/{x}/{y})"), QStringLiteral("xyz"));
        protocol_->addItem(tr("TMS tiles (y from the south)"), QStringLiteral("tms"));
        protocol_->addItem(tr("WMTS (Web-Mercator tile matrix set)"), QStringLiteral("wmts"));
        protocol_->addItem(tr("WMS GetMap"), QStringLiteral("wms"));
        url_->setPlaceholderText(QStringLiteral("https://example.org/tiles/{z}/{x}/{y}.jpg"));
        url_->setToolTip(
            tr("Placeholders: {z} {x} {y} (tiles) or {bbox} {width} {height} {crs} (WMS, "
               "EPSG:3857), and {dataset} {format} {apiKey}. Never put the key itself here."));
    } else {
        protocol_->addItem(tr("REST bounding box (GeoTIFF)"), QStringLiteral("rest_bbox"));
        for (const QString& later : {tr("XYZ / TMS terrain tiles"), tr("OGC WCS"),
                                     tr("OGC API – Coverages"), tr("Cloud-optimised GeoTIFF")}) {
            protocol_->addItem(later);
            disableItem(protocol_, protocol_->count() - 1, tr("Not supported yet."));
        }
        url_->setPlaceholderText(
            QStringLiteral("https://example.org/dem?south={south}&north={north}&west={west}&"
                           "east={east}&dataset={dataset}"));
        url_->setToolTip(
            tr("Placeholders: {dataset} {south} {north} {west} {east} {bbox} {width} {height} "
               "{crs} {format} {apiKey}. Never put the key itself here."));
    }
    if (const int i = protocol_->findData(protocol); i >= 0) {
        protocol_->setCurrentIndex(i);
    }
    if (format_->text() == QLatin1String("GTiff") && tiles) {
        format_->setText(QStringLiteral("image/jpeg"));
    } else if (format_->text() == QLatin1String("image/jpeg") && !tiles) {
        format_->setText(QStringLiteral("GTiff"));
    }
    crs_->setText(tiles ? QStringLiteral("EPSG:3857") : QStringLiteral("EPSG:4326"));
    form_->setRowVisible(tileSize_, tiles);
    form_->setRowVisible(blockSize_, !tiles);
    datasets_->setHorizontalHeaderLabels({tr("Id"), tr("Name"),
                                          tiles ? tr("Zoom levels") : tr("Resolution (m)"),
                                          tr("Vertical datum")});
    datasets_->setColumnHidden(3, tiles);
    for (int row = 0; row < datasets_->rowCount(); ++row) {
        QTableWidgetItem* item = datasets_->item(row, 2);
        if (item == nullptr) {
            continue;
        }
        // Zoom levels "min–max" ⇄ a resolution in metres.
        if (tiles && !item->text().contains(QLatin1Char('-'))) {
            item->setText(QStringLiteral("0-18"));
        } else if (!tiles && item->text().contains(QLatin1Char('-'))) {
            item->setText(QStringLiteral("30"));
        }
    }
}

void ProviderDialog::addDataset(const net::DatasetConfig& d) {
    const int row = datasets_->rowCount();
    datasets_->insertRow(row);
    datasets_->setItem(row, 0, new QTableWidgetItem(qs(d.id)));
    datasets_->setItem(row, 1, new QTableWidgetItem(qs(d.name)));
    datasets_->setItem(row, 2,
                       new QTableWidgetItem(imagery() ? QStringLiteral("%1-%2").arg(d.minZoom).arg(
                                                            d.maxZoom.value_or(18))
                                                      : QString::number(d.resolutionM)));
    datasets_->setItem(row, 3, new QTableWidgetItem(qs(d.verticalDatum)));
}

void ProviderDialog::updateEnabled() {
    keyPlace_->setEnabled(requiresKey_->isChecked());
    keyName_->setEnabled(requiresKey_->isChecked());
}

net::ProviderConfig ProviderDialog::config() const {
    net::ProviderConfig p = start_;
    p.key = key_->text().trimmed().toStdString();
    p.name = name_->text().trimmed().toStdString();
    p.kind = kind_->currentData().toString().toStdString();
    p.protocol = protocol_->currentData().toString().toStdString();
    p.urlTemplate = url_->text().trimmed().toStdString();
    p.format = format_->text().trimmed().toStdString();
    p.crs = imagery() ? "EPSG:3857" : "EPSG:4326";
    p.tileSizePx = tileSize_->value();
    const auto text = [this](int row, int col) {
        const QTableWidgetItem* item = datasets_->item(row, col);
        return item != nullptr ? item->text().trimmed() : QString();
    };
    std::vector<net::DatasetConfig> datasets;
    for (int r = 0; r < datasets_->rowCount(); ++r) {
        net::DatasetConfig d;
        // Keep what the form does not show (coverage, limits, citation…).
        const std::string id = text(r, 0).toStdString();
        if (const net::DatasetConfig* old = start_.dataset(id)) {
            d = *old;
        }
        d.id = id;
        d.name = text(r, 1).toStdString();
        if (imagery()) {
            // "min-max" or just the finest zoom.
            const QStringList zooms = text(r, 2).split(QLatin1Char('-'));
            bool okMin = true;
            bool okMax = false;
            d.minZoom = zooms.size() == 2 ? zooms.front().trimmed().toInt(&okMin) : 0;
            d.maxZoom = zooms.back().trimmed().toInt(&okMax);
            if (!okMin || !okMax) {
                d.maxZoom = -1;  // Reported by validate().
            }
            d.resolutionM = 156543.03392804097 / std::ldexp(1.0, std::max(*d.maxZoom, 0));
            d.verticalDatum.clear();
        } else {
            bool ok = false;
            const double res = text(r, 2).toDouble(&ok);
            d.resolutionM = ok ? res : 0.0;
            d.verticalDatum = text(r, 3).toStdString();
            d.maxZoom.reset();
            d.minZoom = 0;
        }
        if (d.name.empty()) {
            d.name = d.id;
        }
        datasets.push_back(std::move(d));
    }
    p.datasets = std::move(datasets);
    if (p.dataset(p.defaultDataset) == nullptr) {
        p.defaultDataset = p.datasets.empty() ? std::string() : p.datasets.front().id;
    }
    p.requiresKey = requiresKey_->isChecked();
    p.apiKey.location =
        static_cast<net::ApiKeyPlacement::Location>(keyPlace_->currentData().toInt());
    p.apiKey.name = keyName_->text().trimmed().toStdString();
    if (dailyLimit_->value() > 0) {
        p.dailyCallLimit = dailyLimit_->value();
    } else {
        p.dailyCallLimit.reset();
    }
    p.maxConcurrent = concurrent_->value();
    p.blockSizeKm = blockSize_->value();
    p.allowCache = allowCache_->isChecked();
    p.insecureHttp = insecure_->isChecked();
    p.attribution = attribution_->text().trimmed().toStdString();
    p.licence = licence_->text().trimmed().toStdString();
    p.termsUrl = terms_->text().trimmed().toStdString();
    p.origin = "user";
    return p;
}

QString ProviderDialog::validate() {
    const net::ProviderConfig p = config();
    QString problem;
    if (p.key.empty()) {
        problem = tr("The provider needs a key.");
    } else {
        common::Json json = net::toJson(p);
        json["schemaVersion"] = net::kProviderSchemaVersion;
        if (auto parsed = net::parseProvider(json); !parsed) {
            problem = qs(parsed.error().message);
        }
    }
    error_->setText(problem);
    return problem;
}

void ProviderDialog::accept() {
    if (validate().isEmpty()) {
        QDialog::accept();
    }
}

// --- DataProvidersPanel ----------------------------------------------------------------------

DataProvidersPanel::DataProvidersPanel(DataServices& services, QWidget* parent)
    : QWidget(parent), data_(&services) {
    // Providers.
    auto* providersBox = new QGroupBox(tr("Data providers"));
    providersBox->setObjectName(QStringLiteral("DataProvidersBox"));
    auto* pv = new QVBoxLayout(providersBox);
    list_ = new QTreeWidget;
    list_->setObjectName(QStringLiteral("ProvidersList"));
    list_->setHeaderLabels({tr("Provider"), tr("Kind"), tr("API key"), tr("Calls left today")});
    list_->setRootIsDecorated(false);
    list_->setMinimumHeight(90);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    pv->addWidget(list_);

    auto* buttons = new QHBoxLayout;
    add_ = new QPushButton(tr("&Add…"));
    add_->setObjectName(QStringLiteral("AddProviderButton"));
    editButton_ = new QPushButton(tr("&Edit…"));
    editButton_->setObjectName(QStringLiteral("EditProviderButton"));
    duplicate_ = new QPushButton(tr("&Duplicate…"));
    duplicate_->setObjectName(QStringLiteral("DuplicateProviderButton"));
    remove_ = new QPushButton(tr("&Remove"));
    remove_->setObjectName(QStringLiteral("RemoveProviderButton"));
    test_ = new QPushButton(tr("&Test Connection"));
    test_->setObjectName(QStringLiteral("TestConnectionButton"));
    for (QPushButton* b : {add_, editButton_, duplicate_, remove_, test_}) {
        buttons->addWidget(b);
    }
    buttons->addStretch();
    pv->addLayout(buttons);

    details_ = wrapped();
    details_->setObjectName(QStringLiteral("ProviderDetails"));
    details_->setOpenExternalLinks(true);
    pv->addWidget(details_);
    usage_ = wrapped();
    usage_->setObjectName(QStringLiteral("ProviderUsage"));
    pv->addWidget(usage_);

    auto* keyRow = new QHBoxLayout;
    key_ = new QLineEdit;
    key_->setObjectName(QStringLiteral("ApiKeyEdit"));
    key_->setEchoMode(QLineEdit::Password);
    key_->setClearButtonEnabled(true);
    saveKey_ = new QPushButton(tr("Save &Key"));
    saveKey_->setObjectName(QStringLiteral("SaveKeyButton"));
    forgetKey_ = new QPushButton(tr("&Forget Key"));
    forgetKey_->setObjectName(QStringLiteral("ForgetKeyButton"));
    keyRow->addWidget(new QLabel(tr("API key:")));
    keyRow->addWidget(key_, 1);
    keyRow->addWidget(saveKey_);
    keyRow->addWidget(forgetKey_);
    pv->addLayout(keyRow);
    keyStatus_ = wrapped();
    keyStatus_->setObjectName(QStringLiteral("KeyStatusLabel"));
    pv->addWidget(keyStatus_);
    auto* privacy =
        wrapped(tr("Requests carry no personal data, but the requested area is visible to the "
                   "provider. Keys are never written to logs, scenes or missions."));
    privacy->setEnabled(false);
    pv->addWidget(privacy);
    status_ = wrapped();
    status_->setObjectName(QStringLiteral("ProvidersStatusLabel"));
    pv->addWidget(status_);

    // Cache.
    auto* cacheBox = new QGroupBox(tr("Cache"));
    cacheBox->setObjectName(QStringLiteral("CacheBox"));
    auto* cf = new QFormLayout(cacheBox);
    cacheUsage_ = wrapped();
    cacheUsage_->setObjectName(QStringLiteral("CacheUsageLabel"));
    cf->addRow(tr("Usage:"), cacheUsage_);
    cacheLimit_ = new QDoubleSpinBox;
    cacheLimit_->setObjectName(QStringLiteral("CacheLimitSpin"));
    cacheLimit_->setRange(0.1, 2000.0);
    cacheLimit_->setDecimals(1);
    cacheLimit_->setSingleStep(0.5);
    cacheLimit_->setSuffix(QStringLiteral(" GB"));
    cacheLimit_->setValue(static_cast<double>(services.cacheLimit()) / kGb);
    cacheLimit_->setToolTip(
        tr("Least recently used data is removed beyond this size; downloaded areas are kept."));
    cf->addRow(tr("Size &limit:"), cacheLimit_);
    auto* cacheButtons = new QHBoxLayout;
    release_ = new QPushButton(tr("Release Downloaded &Areas"));
    release_->setObjectName(QStringLiteral("ReleasePinnedButton"));
    release_->setToolTip(tr("Lets the size limit remove areas downloaded for offline use."));
    clear_ = new QPushButton(tr("&Clear Cache…"));
    clear_->setObjectName(QStringLiteral("ClearCacheButton"));
    auto* openCache = new QPushButton(tr("&Open Folder"));
    cacheButtons->addWidget(release_);
    cacheButtons->addWidget(clear_);
    cacheButtons->addWidget(openCache);
    cacheButtons->addStretch();
    cf->addRow(QString(), cacheButtons);
    offline_ = new QCheckBox(tr("&Work offline (cached data only)"));
    offline_->setObjectName(QStringLiteral("OfflineCheck"));
    offline_->setChecked(services.offline());
    cf->addRow(QString(), offline_);
    network_ = wrapped();
    network_->setObjectName(QStringLiteral("NetworkStateLabel"));
    cf->addRow(network_);
    proxy_ = new QLineEdit(qs(services.proxy()));
    proxy_->setObjectName(QStringLiteral("ProxyEdit"));
    proxy_->setPlaceholderText(tr("From https_proxy / no_proxy"));
    proxy_->setToolTip(tr("e.g. http://proxy.example:3128. Used from the next start."));
    cf->addRow(tr("&Proxy:"), proxy_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(providersBox);
    layout->addWidget(cacheBox);

    connect(list_, &QTreeWidget::currentItemChanged, this, [this] {
        // A list rebuilt around the same provider keeps the last message.
        if (selectedKey() != shownKey_) {
            shownKey_ = selectedKey();
            showStatus({}, false);
        }
        showSelected();
    });
    connect(saveKey_, &QPushButton::clicked, this, &DataProvidersPanel::saveKey);
    connect(key_, &QLineEdit::returnPressed, this, &DataProvidersPanel::saveKey);
    connect(forgetKey_, &QPushButton::clicked, this, &DataProvidersPanel::forgetKey);
    connect(test_, &QPushButton::clicked, this, &DataProvidersPanel::testConnection);
    connect(add_, &QPushButton::clicked, this, &DataProvidersPanel::addProvider);
    connect(editButton_, &QPushButton::clicked, this, [this] { editProvider(false); });
    connect(duplicate_, &QPushButton::clicked, this, [this] { editProvider(true); });
    connect(remove_, &QPushButton::clicked, this, &DataProvidersPanel::removeProvider);
    connect(cacheLimit_, &QDoubleSpinBox::editingFinished, this, [this] {
        const auto bytes = static_cast<std::uint64_t>(std::llround(cacheLimit_->value() * kGb));
        if (bytes == data_->cacheLimit()) {
            return;
        }
        if (auto ok = data_->setCacheLimit(bytes); !ok) {
            showStatus(qs(ok.error().message), true);
        }
    });
    connect(release_, &QPushButton::clicked, this, [this] {
        if (auto released = data_->releasePinnedAreas(); !released) {
            showStatus(qs(released.error().message), true);
        } else {
            showStatus(tr("%n downloaded piece(s) released.", nullptr, *released), false);
        }
    });
    connect(clear_, &QPushButton::clicked, this, [this] {
        if (!ask(tr("Clear Cache"),
                 tr("Delete the cached data of every provider? Areas downloaded for offline "
                    "use are kept; release them first to delete them too."))) {
            return;
        }
        if (auto ok = data_->clearCache(false); !ok) {
            showStatus(qs(ok.error().message), true);
        } else {
            showStatus(tr("Cache cleared."), false);
        }
    });
    connect(openCache, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdString(data_->cacheDirectory().string())));
    });
    connect(offline_, &QCheckBox::toggled, this, [this](bool on) { data_->setOffline(on); });
    connect(proxy_, &QLineEdit::editingFinished, this, [this] {
        if (proxy_->text().toStdString() != data_->proxy()) {
            data_->setProxy(proxy_->text().toStdString());
            showStatus(tr("The proxy is used from the next start."), false);
        }
    });
    connect(&services, &DataServices::providersChanged, this, &DataProvidersPanel::fillList);
    connect(&services, &DataServices::apiKeysChanged, this, [this] {
        const std::string key = selectedKey();
        fillList();
        selectProvider(key);
    });
    connect(&services, &DataServices::networkSettingsChanged, this, [this] {
        const QSignalBlocker blocker(offline_);
        offline_->setChecked(data_->offline());
        refresh();
    });
    refresh_ = new QTimer(this);
    refresh_->setInterval(3000);
    connect(refresh_, &QTimer::timeout, this, [this] {
        if (isVisible()) {
            refresh();
        }
    });
    refresh_->start();

    fillList();
    updateCache();
}

bool DataProvidersPanel::ask(const QString& title, const QString& text) {
    if (confirm_) {
        return confirm_(title, text);
    }
    return QMessageBox::question(this, title, text) == QMessageBox::Yes;
}

void DataProvidersPanel::showStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

const net::ProviderConfig* DataProvidersPanel::selected() const {
    return data_->provider(selectedKey());
}

std::string DataProvidersPanel::selectedKey() const {
    const QTreeWidgetItem* item = list_->currentItem();
    return item != nullptr ? item->data(0, Qt::UserRole).toString().toStdString() : std::string();
}

bool DataProvidersPanel::selectProvider(const std::string& key) {
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        if (list_->topLevelItem(i)->data(0, Qt::UserRole).toString().toStdString() == key) {
            list_->setCurrentItem(list_->topLevelItem(i));
            return true;
        }
    }
    return false;
}

void DataProvidersPanel::fillList() {
    const std::string current = selectedKey();
    {
        const QSignalBlocker blocker(list_);
        list_->clear();
        for (const net::ProviderConfig& p : data_->providers()) {
            auto* item = new QTreeWidgetItem(list_);
            item->setText(0, qs(p.name) + (p.origin == "user" ? tr(" (yours)") : QString()));
            item->setData(0, Qt::UserRole, qs(p.key));
            item->setText(1, p.kind == "imagery" ? tr("Imagery") : tr("Elevation"));
            QString key = tr("not needed");
            if (p.requiresKey) {
                switch (data_->keyLocation(p.key)) {
                    case KeyLocation::None: key = tr("missing"); break;
                    case KeyLocation::Environment: key = tr("environment"); break;
                    case KeyLocation::Keyring: key = tr("keyring"); break;
                    case KeyLocation::Database: key = tr("database ⚠"); break;
                    case KeyLocation::Session: key = tr("this session"); break;
                }
            }
            item->setText(2, key);
            if (const auto q = data_->quota(p)) {
                item->setText(3, tr("%1 of %2").arg(q->remaining).arg(q->limit));
            } else {
                item->setText(3, tr("no limit"));
            }
        }
    }
    if (!selectProvider(current) && list_->topLevelItemCount() > 0) {
        list_->setCurrentItem(list_->topLevelItem(0));
    }
    showSelected();
}

void DataProvidersPanel::showSelected() {
    const net::ProviderConfig* p = selected();
    const bool user = p != nullptr && p->origin == "user";
    editButton_->setEnabled(user);
    editButton_->setToolTip(user || p == nullptr
                                ? QString()
                                : tr("Built-in providers cannot be changed; duplicate it."));
    duplicate_->setEnabled(p != nullptr);
    remove_->setEnabled(user);
    test_->setEnabled(p != nullptr && !testing_ &&
                      (p->protocol == "rest_bbox" || net::isTileProtocol(p->protocol)));
    if (p == nullptr) {
        details_->clear();
        usage_->clear();
        keyStatus_->clear();
        key_->setEnabled(false);
        saveKey_->setEnabled(false);
        forgetKey_->setEnabled(false);
        return;
    }

    QStringList lines;
    lines << tr("<b>%1</b> (<code>%2</code>) · %3 · %4")
                 .arg(qs(p->name).toHtmlEscaped(), qs(p->key).toHtmlEscaped(),
                      user ? tr("your provider") : tr("built-in"),
                      p->protocol == "rest_bbox" ? tr("REST bounding box")
                                                 : qs(p->protocol).toUpper());
    lines << tr("URL: <code>%1</code>").arg(qs(p->urlTemplate).toHtmlEscaped());
    QStringList datasets;
    for (const net::DatasetConfig& d : p->datasets) {
        if (p->isImagery()) {
            datasets << tr("%1 — %2 (zoom %3 … %4)%5")
                            .arg(qs(d.id).toHtmlEscaped(), qs(d.name).toHtmlEscaped())
                            .arg(d.minZoom)
                            .arg(d.maxZoom.value_or(net::kMaxImageryZoom))
                            .arg(d.verified ? QString() : tr(" · request format not verified"));
            continue;
        }
        datasets << tr("%1 — %2 (%3 m%4)%5")
                        .arg(qs(d.id).toHtmlEscaped(), qs(d.name).toHtmlEscaped())
                        .arg(d.resolutionM, 0, 'g', 4)
                        .arg(d.verticalDatum.empty() ? QString()
                                                     : QStringLiteral(", ") + qs(d.verticalDatum))
                        .arg(d.verified ? QString() : tr(" · request format not verified"));
    }
    lines << tr("Datasets: %1").arg(datasets.join(QStringLiteral("; ")));
    QStringList limits;
    if (p->dailyCallLimit) {
        limits << tr("%1 calls per 24 h").arg(*p->dailyCallLimit);
    }
    limits << tr("%1 simultaneous").arg(p->maxConcurrent)
           << tr("blocks of %1 km").arg(p->blockSizeKm, 0, 'g', 3);
    if (!p->allowCache) {
        limits << tr("no caching");
    }
    lines << tr("Limits: %1").arg(limits.join(QStringLiteral(", ")));
    if (p->insecureHttp) {
        lines << tr("⚠ Plain HTTP allowed: requests and the key are not encrypted.");
    }
    if (!p->attribution.empty()) {
        lines << tr("Attribution: %1").arg(qs(p->attribution).toHtmlEscaped());
    }
    if (!p->licence.empty()) {
        lines << tr("Licence: %1").arg(qs(p->licence).toHtmlEscaped());
    }
    if (!p->termsUrl.empty()) {
        lines << tr("Terms: <a href=\"%1\">%1</a>").arg(qs(p->termsUrl).toHtmlEscaped());
    }
    details_->setText(lines.join(QStringLiteral("<br>")));

    // Key (FR-NET-07): the stored key itself is never shown.
    const KeyLocation where = data_->keyLocation(p->key);
    const bool fromEnvironment = where == KeyLocation::Environment;
    key_->setEnabled(p->requiresKey && !fromEnvironment);
    saveKey_->setEnabled(p->requiresKey && !fromEnvironment);
    forgetKey_->setEnabled(p->requiresKey && !fromEnvironment && where != KeyLocation::None);
    key_->clear();
    key_->setPlaceholderText(where == KeyLocation::None ? tr("Paste your personal key")
                                                        : tr("A key is set; type to replace it"));
    QString status;
    if (!p->requiresKey) {
        status = tr("This provider needs no key.");
    } else {
        switch (where) {
            case KeyLocation::None:
                status = tr("No key: get a free personal key from %1 and enter it here.")
                             .arg(qs(p->name).toHtmlEscaped());
                break;
            case KeyLocation::Environment:
                status = tr("Key from the environment variable <code>%1</code>.")
                             .arg(qs(DataServices::keyVariable(p->key)));
                break;
            case KeyLocation::Keyring: status = tr("Key stored in the system keyring."); break;
            case KeyLocation::Database:
                status = tr("⚠ Key stored in the user database (readable only by you), because "
                            "the system keyring is not available%1.")
                             .arg(data_->keyringProblem().empty()
                                      ? QString()
                                      : QStringLiteral(": ") +
                                            qs(data_->keyringProblem()).toHtmlEscaped());
                break;
            case KeyLocation::Session:
                status = tr("Key kept for this session only (nowhere to store it).");
                break;
        }
    }
    keyStatus_->setText(status);
    refresh();
}

void DataProvidersPanel::refresh() {
    updateCache();
    const net::ProviderConfig* p = selected();
    if (p == nullptr) {
        return;
    }
    const ProviderUsageInfo u = data_->usage(*p);
    QStringList lines;
    QString day = tr("Last 24 h: %n call(s)", nullptr, u.last24h.calls);
    if (u.last24h.errors > 0) {
        day += tr(", %n failed", nullptr, u.last24h.errors);
    }
    day += QStringLiteral(", ") + formatBytes(u.last24h.bytes);
    if (u.quota) {
        day += tr(" · %1 of %2 calls left").arg(u.quota->remaining).arg(u.quota->limit);
        if (u.quota->exhausted() && u.quota->nextFreeAtUnixS) {
            day += tr(" (cached data only until %1)")
                       .arg(QDateTime::fromSecsSinceEpoch(*u.quota->nextFreeAtUnixS)
                                .toLocalTime()
                                .toString(QStringLiteral("HH:mm")));
        }
    }
    lines << day;
    QString session = tr("This session: %1 from the cache, %2 request(s)")
                          .arg(u.session.cacheHits)
                          .arg(u.session.requests);
    if (const auto rate = u.session.hitRate()) {
        session += tr(" (%1 % cache hits)").arg(std::lround(*rate * 100.0));
    }
    if (u.session.failures > 0) {
        session += tr(", %1 failed").arg(u.session.failures);
    }
    lines << session;
    lines << tr("Cached: %1 piece(s), %2; downloaded areas %3")
                 .arg(u.cache.entries)
                 .arg(formatBytes(u.cache.bytes), formatBytes(u.cache.pinnedBytes));
    if (u.circuitOpen) {
        lines << tr("⚠ Requests are paused after repeated failures.");
    }
    usage_->setText(lines.join(QStringLiteral("<br>")));
}

void DataProvidersPanel::updateCache() {
    std::uint64_t bytes = 0;
    std::uint64_t pinned = 0;
    int pieces = 0;
    for (const net::CacheUsage& c : data_->cacheUsage()) {
        bytes += c.bytes;
        pinned += c.pinnedBytes;
        pieces += c.entries;
    }
    cacheUsage_->setText(
        tr("%1 of %2 in %n piece(s); downloaded areas %3", nullptr, pieces)
            .arg(formatBytes(bytes), formatBytes(data_->cacheLimit()), formatBytes(pinned)));
    release_->setEnabled(pinned > 0);
    clear_->setEnabled(bytes > pinned);
    if (data_->offline()) {
        network_->setText(tr("Offline: only cached data is used."));
    } else if (!data_->http().networkReachable()) {
        network_->setText(tr("⚠ The network is unreachable: using cached data until it is back."));
    } else {
        network_->setText(tr("Online."));
    }
}

void DataProvidersPanel::saveKey() {
    const net::ProviderConfig* p = selected();
    const std::string key = key_->text().trimmed().toStdString();
    if (p == nullptr || key.empty()) {
        return;
    }
    const std::string providerKey = p->key;
    auto where = data_->storeApiKey(providerKey, key);
    key_->clear();
    if (!where) {
        showStatus(qs(where.error().message), true);
        return;
    }
    showStatus(tr("Key saved in %1.").arg(qs(keyLocationName(*where))),
               *where == KeyLocation::Database || *where == KeyLocation::Session);
}

void DataProvidersPanel::forgetKey() {
    const net::ProviderConfig* p = selected();
    if (p == nullptr) {
        return;
    }
    if (auto ok = data_->storeApiKey(p->key, {}); !ok) {
        showStatus(qs(ok.error().message), true);
        return;
    }
    showStatus(tr("Key removed."), false);
}

void DataProvidersPanel::testConnection() {
    const net::ProviderConfig* p = selected();
    if (p == nullptr || testing_) {
        return;
    }
    if (const auto q = data_->quota(*p)) {
        if (!ask(tr("Test Connection"),
                 tr("The test uses 1 of your %1 remaining calls to %2 today. Continue?")
                     .arg(q->remaining)
                     .arg(qs(p->name)))) {
            return;
        }
    }
    testing_ = true;
    test_->setEnabled(false);
    showStatus(tr("Testing %1…").arg(qs(p->name)), false);
    const std::string key = p->key;
    data_->testConnection(*p, p->defaultDataset, [this, key](const ConnectionTest& t) {
        testing_ = false;
        const QString when = tr("%1 ms").arg(t.latency.count());
        if (t.ok) {
            showStatus(
                tr("Connection OK: %1, %2, %3.").arg(when, formatBytes(t.bytes), qs(t.message)),
                false);
        } else {
            showStatus(tr("Connection failed after %1: %2").arg(when, qs(t.message)), true);
        }
        const std::string current = selectedKey();
        fillList();
        selectProvider(current.empty() ? key : current);
    });
}

void DataProvidersPanel::addProvider() {
    net::ProviderConfig blank;
    blank.key = "my-dem";
    blank.name = tr("My elevation server").toStdString();
    blank.format = "GTiff";
    blank.contentTypes = {"image/tiff", "application/octet-stream"};
    blank.datasets.push_back(net::DatasetConfig{.id = "dem", .name = "DEM", .resolutionM = 30.0});
    blank.defaultDataset = "dem";
    blank.dailyCallLimit.reset();
    blank.origin = "user";
    const auto edited = edit_ ? edit_(blank, true) : [&]() -> std::optional<net::ProviderConfig> {
        ProviderDialog dialog(blank, true, this);
        return dialog.exec() == QDialog::Accepted ? std::optional(dialog.config()) : std::nullopt;
    }();
    if (!edited) {
        return;
    }
    if (data_->provider(edited->key) != nullptr) {
        showStatus(tr("A provider with the key \"%1\" exists already.").arg(qs(edited->key)), true);
        return;
    }
    if (auto ok = data_->saveProvider(*edited); !ok) {
        showStatus(qs(ok.error().message), true);
        return;
    }
    selectProvider(edited->key);
    showStatus(tr("%1 added.").arg(qs(edited->name)), false);
}

void DataProvidersPanel::editProvider(bool duplicate) {
    const net::ProviderConfig* p = selected();
    if (p == nullptr || (!duplicate && p->origin != "user")) {
        return;
    }
    net::ProviderConfig start = *p;
    if (duplicate) {
        start.key += "-copy";
        start.name += tr(" (copy)").toStdString();
        start.origin = "user";
    }
    const auto edited =
        edit_ ? edit_(start, duplicate) : [&]() -> std::optional<net::ProviderConfig> {
        ProviderDialog dialog(start, duplicate, this);
        return dialog.exec() == QDialog::Accepted ? std::optional(dialog.config()) : std::nullopt;
    }();
    if (!edited) {
        return;
    }
    if (duplicate && data_->provider(edited->key) != nullptr) {
        showStatus(tr("A provider with the key \"%1\" exists already.").arg(qs(edited->key)), true);
        return;
    }
    if (auto ok = data_->saveProvider(*edited); !ok) {
        showStatus(qs(ok.error().message), true);
        return;
    }
    selectProvider(edited->key);
    showStatus(tr("%1 saved.").arg(qs(edited->name)), false);
}

void DataProvidersPanel::removeProvider() {
    const net::ProviderConfig* p = selected();
    if (p == nullptr || p->origin != "user") {
        return;
    }
    const std::string key = p->key;
    const QString name = qs(p->name);
    if (!ask(tr("Remove Data Provider"),
             tr("Remove %1? Its API key and cached data are deleted too; missions using it "
                "become invalid.")
                 .arg(name))) {
        return;
    }
    if (auto ok = data_->removeProvider(key, true); !ok) {
        showStatus(qs(ok.error().message), true);
        return;
    }
    showStatus(tr("%1 removed.").arg(name), false);
}

}  // namespace ds::app
