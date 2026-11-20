// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "ds/net/provider.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QTreeWidget;

/// @file
/// @brief Settings › Data providers and Settings › Cache (SPECIFICATION §4.11, FR-NET-02 …
/// FR-NET-11, FR-TER-O02).
/// @ingroup app

namespace ds::app {

class DataServices;

/// @brief Editor of a user-defined bounding-box provider (FR-TER-O02): key, name, protocol,
/// URL template, datasets, API key placement, limits, attribution and licence.
/// @details Fields the form does not show (content types, quota error texts, coverage …) are
/// kept from the provider it starts from. *OK* validates like a provider file.
class ProviderDialog final : public QDialog {
    Q_OBJECT

public:
    /// @param start Provider to edit or to start from.
    /// @param isNew True for *Add* / *Duplicate*: the key can be edited.
    /// @param parent Parent widget.
    ProviderDialog(net::ProviderConfig start, bool isNew, QWidget* parent = nullptr);

    /// @brief The provider of the form (origin `user`).
    [[nodiscard]] net::ProviderConfig config() const;
    /// @brief Validates the form; empty if it is valid, else the first problem (also shown
    /// in the dialog).
    QString validate();

    /// @brief Accepts only a valid form.
    void accept() override;

private:
    void addDataset(const net::DatasetConfig& dataset);
    void updateEnabled();

    net::ProviderConfig start_;
    QLineEdit* key_ = nullptr;
    QLineEdit* name_ = nullptr;
    QComboBox* kind_ = nullptr;
    QComboBox* protocol_ = nullptr;
    QLineEdit* url_ = nullptr;
    QLineEdit* format_ = nullptr;
    QTableWidget* datasets_ = nullptr;
    QCheckBox* requiresKey_ = nullptr;
    QComboBox* keyPlace_ = nullptr;
    QLineEdit* keyName_ = nullptr;
    QSpinBox* dailyLimit_ = nullptr;
    QSpinBox* concurrent_ = nullptr;
    QDoubleSpinBox* blockSize_ = nullptr;
    QCheckBox* insecure_ = nullptr;
    QCheckBox* allowCache_ = nullptr;
    QLineEdit* attribution_ = nullptr;
    QLineEdit* licence_ = nullptr;
    QLineEdit* terms_ = nullptr;
    QLabel* error_ = nullptr;
};

/// @brief Settings › Data providers (list, key, test connection, usage, editor) and
/// Settings › Cache (usage, limit, pinned areas, clear, offline mode, proxy).
class DataProvidersPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Asks a yes/no question; true ⇒ yes.
    using Confirm = std::function<bool(const QString& title, const QString& text)>;
    /// @brief Edits a provider; nullopt ⇒ cancelled.
    using EditProvider =
        std::function<std::optional<net::ProviderConfig>(const net::ProviderConfig&, bool isNew)>;

    /// @brief Panel for @p services, which must outlive it.
    explicit DataProvidersPanel(DataServices& services, QWidget* parent = nullptr);

    /// @brief Replaces the questions (tests).
    void setConfirm(Confirm confirm) { confirm_ = std::move(confirm); }
    /// @brief Replaces the provider editor (tests).
    void setEditProvider(EditProvider edit) { edit_ = std::move(edit); }

    /// @brief Selects the provider @p key in the list; false if it is not there.
    bool selectProvider(const std::string& key);
    /// @brief Key of the selected provider ("" if none).
    [[nodiscard]] std::string selectedKey() const;
    /// @brief Re-reads usage and cache figures.
    void refresh();

private:
    void fillList();
    void showSelected();
    void saveKey();
    void forgetKey();
    void testConnection();
    void addProvider();
    void editProvider(bool duplicate);
    void removeProvider();
    void showStatus(const QString& text, bool error);
    void updateCache();
    [[nodiscard]] const net::ProviderConfig* selected() const;
    bool ask(const QString& title, const QString& text);

    DataServices* data_;
    Confirm confirm_;
    EditProvider edit_;
    QTreeWidget* list_ = nullptr;
    QLabel* details_ = nullptr;
    QLabel* usage_ = nullptr;
    QLineEdit* key_ = nullptr;
    QPushButton* saveKey_ = nullptr;
    QPushButton* forgetKey_ = nullptr;
    QLabel* keyStatus_ = nullptr;
    QPushButton* test_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* duplicate_ = nullptr;
    QPushButton* remove_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* cacheUsage_ = nullptr;
    QDoubleSpinBox* cacheLimit_ = nullptr;
    QPushButton* release_ = nullptr;
    QPushButton* clear_ = nullptr;
    QCheckBox* offline_ = nullptr;
    QLabel* network_ = nullptr;
    QLineEdit* proxy_ = nullptr;
    QTimer* refresh_ = nullptr;
    std::string shownKey_;  // Provider whose details are shown.
    bool testing_ = false;
};

/// @brief "1.5 MB", "820 kB", "12 B".
[[nodiscard]] QString formatBytes(std::uint64_t bytes);

}  // namespace ds::app
