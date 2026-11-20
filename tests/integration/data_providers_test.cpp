// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Settings › Data providers and Settings › Cache (M4 step 5) against the mock provider of
// tests/support/online_fixtures.hpp: API keys in the keyring or, without one, in the user
// database; Test connection and usage; user-defined providers in the Terrain tab and the
// mission loader; cache usage, pinned areas, offline mode and saved settings (FR-NET-02 …
// FR-NET-11, FR-TER-O02). The real OS keyring is never touched: tests use MemoryKeyring.

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "ds/app/asset_paths.hpp"
#include "ds/app/data_providers_panel.hpp"
#include "ds/app/data_services.hpp"
#include "ds/app/keyring.hpp"
#include "ds/app/mission_library.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/terrain_panel.hpp"
#include "ds/app/user_storage.hpp"
#include "ds/common/json.hpp"
#include "ds/storage/provider_store.hpp"

#include "online_fixtures.hpp"

namespace ds::app {
namespace {

namespace fs = std::filesystem;

class DataProvidersTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(home_.isValid());
        auto storage = openUserStorage(std::nullopt, homeDir());
        ASSERT_TRUE(storage.has_value());
        storage_ = std::move(*storage);
        fs::create_directories(homeDir() / "providers");
        std::ofstream(homeDir() / "providers" / "mockdem.json") << server_.providerJson();
        open();
    }

    void TearDown() override { data_.reset(); }

    // (Re)starts the data services, as a restart of the application would.
    void open(std::shared_ptr<Keyring> keyring = nullptr) {
        data_.reset();
        auto data = DataServices::create({.dataDirectory = homeDir() / "data",
                                          .providerPresets = homeDir() / "providers",
                                          .userStorage = storage_.service.get(),
                                          .keyring = std::move(keyring),
                                          .keysFromEnvironment = false});
        ASSERT_TRUE(data.has_value()) << data.error().message;
        data_ = std::move(*data);
        data_->setConfirmDownload([](QWidget*, const DownloadRequest&) { return true; });
    }

    [[nodiscard]] fs::path homeDir() const { return fs::path(home_.path().toStdString()); }

    std::optional<std::string> storedCredential(const std::string& key) {
        auto r = storage_.service
                     ->submit([key](storage::DbConnection& c) {
                         return storage::ProviderStore(c).credential(key);
                     })
                     .get();
        EXPECT_TRUE(r.has_value());
        return r ? *r : std::nullopt;
    }

    template <typename T>
    T* child(QWidget& parent, const char* name) {
        T* w = parent.findChild<T*>(QString::fromLatin1(name));
        EXPECT_NE(w, nullptr) << name;
        return w;
    }

    test::MockDemServer server_;
    QTemporaryDir home_;
    UserStorage storage_;
    std::unique_ptr<DataServices> data_;
};

TEST_F(DataProvidersTest, KeysGoToTheKeyringElseToThePrivateDatabaseWithAWarning) {
    // No keyring: the database copy, file mode 0600, with a warning in the panel.
    {
        DataProvidersPanel panel(*data_);
        ASSERT_TRUE(panel.selectProvider("mockdem"));
        child<QLineEdit>(panel, "ApiKeyEdit")->setText(QStringLiteral("db-key"));
        child<QPushButton>(panel, "SaveKeyButton")->click();
        EXPECT_EQ(data_->keyLocation("mockdem"), KeyLocation::Database);
        EXPECT_EQ(storedCredential("mockdem"), "db-key");
        EXPECT_TRUE(child<QLabel>(panel, "KeyStatusLabel")->text().contains("user database"));
        EXPECT_TRUE(child<QLineEdit>(panel, "ApiKeyEdit")->text().isEmpty());  // Never shown.
        const fs::path db = storage_.service->info().databaseFile;
        EXPECT_EQ(fs::status(db).permissions() & fs::perms::all,
                  fs::perms::owner_read | fs::perms::owner_write);
    }
    open();
    EXPECT_EQ(data_->keyLocation("mockdem"), KeyLocation::Database);
    EXPECT_TRUE(data_->hasApiKey("mockdem"));

    // With a keyring the key moves there and the database copy is removed.
    auto keyring = std::make_shared<MemoryKeyring>();
    open(keyring);
    EXPECT_EQ(data_->keyLocation("mockdem"), KeyLocation::Database);  // Still the old copy.
    auto stored = data_->storeApiKey("mockdem", "ring-key");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, KeyLocation::Keyring);
    EXPECT_EQ(keyring->peek("provider/mockdem"), "ring-key");
    EXPECT_FALSE(storedCredential("mockdem").has_value());
    open(keyring);
    EXPECT_EQ(data_->keyLocation("mockdem"), KeyLocation::Keyring);

    DataProvidersPanel panel(*data_);
    ASSERT_TRUE(panel.selectProvider("mockdem"));
    EXPECT_TRUE(child<QLabel>(panel, "KeyStatusLabel")->text().contains("system keyring"));
    child<QPushButton>(panel, "ForgetKeyButton")->click();
    EXPECT_FALSE(data_->hasApiKey("mockdem"));
    EXPECT_FALSE(keyring->peek("provider/mockdem").has_value());
    EXPECT_EQ(child<QTreeWidget>(panel, "ProvidersList")->currentItem()->text(2),
              QStringLiteral("missing"));
}

TEST_F(DataProvidersTest, TestConnectionUsesOneCallAndShowsTheUsage) {
    DataProvidersPanel panel(*data_);
    QStringList questions;
    bool accept = true;
    panel.setConfirm([&](const QString&, const QString& text) {
        questions << text;
        return accept;
    });
    ASSERT_TRUE(panel.selectProvider("mockdem"));
    auto* status = child<QLabel>(panel, "ProvidersStatusLabel");
    auto* test = child<QPushButton>(panel, "TestConnectionButton");

    // Without a key the provider is never called.
    test->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return status->text().startsWith("Connection"); }))
        << status->text().toStdString() << " questions " << questions.size();
    EXPECT_TRUE(status->text().contains("API key")) << status->text().toStdString();
    EXPECT_EQ(server_.requestCount(), 0U);

    ASSERT_TRUE(data_->storeApiKey("mockdem", "secret"));
    accept = false;
    test->click();  // FR-NET-11: warned that it uses a call; declined.
    EXPECT_TRUE(questions.back().contains("1 of your 50 remaining calls"))
        << questions.back().toStdString();
    EXPECT_EQ(server_.requestCount(), 0U);

    accept = true;
    test->click();
    EXPECT_FALSE(test->isEnabled());
    ASSERT_TRUE(QTest::qWaitFor([&] { return status->text().startsWith("Connection OK"); }))
        << status->text().toStdString();
    EXPECT_TRUE(status->text().contains(" ms")) << status->text().toStdString();
    EXPECT_TRUE(status->text().contains("heights")) << status->text().toStdString();
    EXPECT_TRUE(test->isEnabled());
    ASSERT_EQ(server_.requestCount(), 1U);
    const auto request = server_.requests().front().target;
    EXPECT_NE(request.find("south=46.495"), std::string::npos) << request;

    // FR-NET-09: calls of the last 24 h against the budget; nothing was cached.
    const QString usage = child<QLabel>(panel, "ProviderUsage")->text();
    EXPECT_TRUE(usage.contains("1 call(s)")) << usage.toStdString();
    EXPECT_TRUE(usage.contains("49 of 50 calls left")) << usage.toStdString();
    EXPECT_TRUE(usage.contains("0 from the cache, 1 request(s)")) << usage.toStdString();
    EXPECT_TRUE(data_->cacheUsage().empty());
    EXPECT_EQ(child<QTreeWidget>(panel, "ProvidersList")->currentItem()->text(3),
              QStringLiteral("49 of 50"));

    server_.setBroken(true);
    test->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { return status->text().startsWith("Connection failed"); }))
        << status->text().toStdString();
    EXPECT_TRUE(status->text().contains("500")) << status->text().toStdString();
}

TEST_F(DataProvidersTest, UserProvidersAreEditedSavedAndUsedEverywhere) {
    // The editor validates like a provider file.
    net::ProviderConfig start = *data_->provider("mockdem");
    {
        ProviderDialog dialog(start, true);
        dialog.findChild<QLineEdit*>(QStringLiteral("ProviderKeyEdit"))->setText("my-dem");
        dialog.findChild<QLineEdit*>(QStringLiteral("ProviderNameEdit"))->setText("My DEM");
        EXPECT_TRUE(dialog.validate().isEmpty()) << dialog.validate().toStdString();
        const net::ProviderConfig edited = dialog.config();
        EXPECT_EQ(edited.key, "my-dem");
        EXPECT_EQ(edited.origin, "user");
        EXPECT_EQ(edited.quotaErrorPatterns, start.quotaErrorPatterns);  // Kept.
        ASSERT_EQ(edited.datasets.size(), 1U);
        EXPECT_EQ(edited.datasets[0].attribution, "Test DEM © nobody");
        dialog.findChild<QLineEdit*>(QStringLiteral("UrlTemplateEdit"))->clear();
        EXPECT_TRUE(dialog.validate().contains("urlTemplate")) << dialog.validate().toStdString();
    }

    MissionsController missions(
        MissionFolders{.assetDir = findAssetDirectory(), .userDir = {}, .libraryDir = {}},
        storage_.service.get(), data_.get());
    SettingsController settings(*storage_.service);
    SceneController scene(&settings, findAssetDirectory(), data_.get());
    TerrainPanel terrain(scene);
    auto* providers = child<QComboBox>(terrain, "ProviderCombo");
    EXPECT_EQ(providers->count(), 1);

    DataProvidersPanel panel(*data_);
    panel.setConfirm([](const QString&, const QString&) { return true; });
    panel.setEditProvider([](const net::ProviderConfig& p, bool isNew) {
        EXPECT_TRUE(isNew);
        net::ProviderConfig mine = p;
        mine.key = "my-dem";
        mine.name = "My DEM";
        return std::optional(mine);
    });
    ASSERT_TRUE(panel.selectProvider("mockdem"));
    EXPECT_FALSE(child<QPushButton>(panel, "EditProviderButton")->isEnabled());  // Built-in.
    EXPECT_FALSE(child<QPushButton>(panel, "RemoveProviderButton")->isEnabled());
    child<QPushButton>(panel, "DuplicateProviderButton")->click();
    ASSERT_NE(data_->provider("my-dem"), nullptr);
    EXPECT_EQ(panel.selectedKey(), "my-dem");
    EXPECT_TRUE(child<QPushButton>(panel, "RemoveProviderButton")->isEnabled());

    // The Terrain tab offers it, and missions may name it (FR-TER-O02).
    EXPECT_EQ(providers->count(), 2);
    EXPECT_GE(providers->findText(QStringLiteral("My DEM")), 0);
    EXPECT_TRUE(missions.folders().extraProviders.contains("my-dem"));

    // Saved in the user database: still there after a restart.
    open();
    ASSERT_NE(data_->provider("my-dem"), nullptr);
    EXPECT_EQ(data_->provider("my-dem")->origin, "user");

    DataProvidersPanel again(*data_);
    again.setConfirm([](const QString&, const QString&) { return true; });
    ASSERT_TRUE(again.selectProvider("my-dem"));
    child<QPushButton>(again, "RemoveProviderButton")->click();
    EXPECT_EQ(data_->provider("my-dem"), nullptr);
    open();
    EXPECT_EQ(data_->provider("my-dem"), nullptr);
}

TEST_F(DataProvidersTest, CacheUsagePinnedAreasOfflineModeAndSavedSettings) {
    ASSERT_TRUE(data_->storeApiKey("mockdem", "secret"));
    terrain::OnlineTerrainRequest request;
    request.provider = *data_->provider("mockdem");
    request.origin = {41.5934, 1.8376};
    request.extent = terrain::Extent{2000.0, 2000.0};
    auto downloaded = ensureOnlineData(nullptr, *data_, request);
    ASSERT_TRUE(downloaded.has_value()) << downloaded.error().message;
    ASSERT_TRUE(*downloaded);

    DataProvidersPanel panel(*data_);
    panel.setConfirm([](const QString&, const QString&) { return true; });
    auto* usage = child<QLabel>(panel, "CacheUsageLabel");
    EXPECT_TRUE(usage->text().contains("in 1 piece(s)")) << usage->text().toStdString();
    auto* release = child<QPushButton>(panel, "ReleasePinnedButton");
    auto* clear = child<QPushButton>(panel, "ClearCacheButton");
    EXPECT_TRUE(release->isEnabled());
    EXPECT_FALSE(clear->isEnabled());  // Only a downloaded area, which Clear keeps.

    release->click();
    EXPECT_TRUE(child<QLabel>(panel, "ProvidersStatusLabel")->text().contains("1 downloaded"));
    EXPECT_EQ(data_->cacheUsage().at(0).pinnedEntries, 0);
    EXPECT_FALSE(release->isEnabled());
    ASSERT_TRUE(clear->isEnabled());
    clear->click();
    EXPECT_TRUE(data_->cacheUsage().empty());
    EXPECT_TRUE(usage->text().contains("in 0 piece(s)")) << usage->text().toStdString();

    // Offline mode, cache limit and proxy are saved (FR-NET-05, 06, 08).
    auto* offline = child<QCheckBox>(panel, "OfflineCheck");
    offline->setChecked(true);
    EXPECT_TRUE(data_->http().offline());
    EXPECT_TRUE(child<QLabel>(panel, "NetworkStateLabel")->text().startsWith("Offline"));
    auto* limit = child<QDoubleSpinBox>(panel, "CacheLimitSpin");
    EXPECT_DOUBLE_EQ(limit->value(), 5.0);
    limit->setValue(2.0);
    emit limit->editingFinished();
    EXPECT_EQ(data_->cacheLimit(), std::uint64_t{2} << 30U);
    auto* proxy = child<QLineEdit>(panel, "ProxyEdit");
    proxy->setText(QStringLiteral("http://proxy.invalid:3128"));
    emit proxy->editingFinished();

    open();
    EXPECT_TRUE(data_->offline());
    EXPECT_EQ(data_->cacheLimit(), std::uint64_t{2} << 30U);
    EXPECT_EQ(data_->proxy(), "http://proxy.invalid:3128");
    auto again = ensureOnlineData(nullptr, *data_, request);  // Offline and not cached.
    ASSERT_FALSE(again.has_value());
    EXPECT_NE(again.error().message.find("offline"), std::string::npos);
    EXPECT_EQ(server_.requestCount(), 1U);
}

}  // namespace
}  // namespace ds::app
