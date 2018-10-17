/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/files/scoped_temp_dir.h"
#include "brave/components/brave_sync/client/bookmark_change_processor.h"
#include "brave/components/brave_sync/client/brave_sync_client_impl.h"
#include "brave/components/brave_sync/client/client_ext_impl_data.h"
#include "brave/components/brave_sync/brave_sync_service_impl.h"
#include "brave/components/brave_sync/brave_sync_service_factory.h"
#include "brave/components/brave_sync/brave_sync_service_observer.h"
#include "brave/components/brave_sync/jslib_messages.h"
#include "brave/components/brave_sync/test_util.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/test/test_bookmark_client.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

      // Separated to easier move or remove
      #include "base/strings/utf_string_conversions.h"
      #include "brave/components/brave_sync/jslib_const.h"
      // Deps brave/browser/BUILD.gn, already in brave/test/BUILD.gn
      #include "brave/browser/bookmarks/brave_bookmark_client.h"
      #include "components/bookmarks/browser/bookmark_model.h"
      #include "components/bookmarks/browser/bookmark_utils.h"

// npm run test -- brave_unit_tests --filter=BraveSyncServiceTest.*

// BraveSyncClient::methods
// Name                     | Covered by
//------------------------------------
// SetSyncToBrowserHandler  |
// GetSyncToBrowserHandler  |
// SendGotInitData          |
// SendFetchSyncRecords     |
// SendFetchSyncDevices     |
// SendResolveSyncRecords   |
// SendSyncRecords          | BookmarkAdded
// SendDeleteSyncUser       | ?
// SendDeleteSyncCategory   | ?
// SendGetBookmarksBaseOrder|
// SendGetBookmarkOrder     | BookmarkAdded
// NeedSyncWords            | ?
// NeedBytesFromSyncWords   | ?
// OnExtensionInitialized   |

using testing::_;
using testing::AtLeast;
using namespace brave_sync;

class MockBraveSyncClient : public BraveSyncClient {
 public:
  MockBraveSyncClient() {}

  MOCK_METHOD0(sync_message_handler, SyncMessageHandler*());
  MOCK_METHOD4(SendGotInitData, void(const Uint8Array& seed,
    const Uint8Array& device_id, const client_data::Config& config,
    const std::string& sync_words));
  MOCK_METHOD3(SendFetchSyncRecords, void(
    const std::vector<std::string>& category_names, const base::Time& startAt,
    const int max_records));
  MOCK_METHOD0(SendFetchSyncDevices, void());
  MOCK_METHOD2(SendResolveSyncRecords, void(const std::string& category_name,
    std::unique_ptr<SyncRecordAndExistingList> list));
  MOCK_METHOD2(SendSyncRecords, void (const std::string& category_name,
    const RecordsList& records));
  MOCK_METHOD0(SendDeleteSyncUser, void());
  MOCK_METHOD1(SendDeleteSyncCategory, void(const std::string& category_name));
  MOCK_METHOD2(SendGetBookmarksBaseOrder, void(const std::string& device_id,
    const std::string& platform));
  MOCK_METHOD3(SendGetBookmarkOrder, void(const std::string& prevOrder,
    const std::string& nextOrder, const std::string& parent_order));
  MOCK_METHOD1(NeedSyncWords, void(const std::string& seed));
  MOCK_METHOD1(NeedBytesFromSyncWords, void(const std::string& words));
  MOCK_METHOD0(OnExtensionInitialized, void());
  MOCK_METHOD0(OnSyncEnabledChanged, void());
};

class MockBraveSyncServiceObserver : public BraveSyncServiceObserver {
 public:
  MockBraveSyncServiceObserver() {}

  MOCK_METHOD1(OnSyncStateChanged, void(BraveSyncService*));
  MOCK_METHOD2(OnHaveSyncWords, void(BraveSyncService*, const std::string&));
  MOCK_METHOD2(OnLogMessage, void(BraveSyncService*, const std::string&));
};

namespace brave_sync {
  extern int64_t deleted_node_id;
}

std::unique_ptr<KeyedService> BuildFakeBookmarkModelForTests(
    content::BrowserContext* context) {
  // Don't need context, unless we have more than one profile
  using namespace bookmarks;

  std::unique_ptr<TestBookmarkClient> client(new TestBookmarkClient());
  BookmarkPermanentNodeList extra_nodes;
  brave_sync::deleted_node_id = 0xDE1E7ED40DE;
  auto node = std::make_unique<BookmarkPermanentNode>(brave_sync::deleted_node_id);
  extra_nodes.push_back(std::move(node));
  client->SetExtraNodesToLoad(std::move(extra_nodes));

  std::unique_ptr<BookmarkModel> model(
      TestBookmarkClient::CreateModelWithClient(std::move(client)));
  return model;
}

namespace brave_sync {
class BraveSyncServiceImplTestAccess {
public:
  static void PretendBackgroundSyncStarted(BraveSyncService* service) {
    BraveSyncServiceImplTestAccess::GetImpl(service)->BackgroundSyncStarted();
  }

  static void PretendBackgroundSyncStopped(BraveSyncService* service) {
    BraveSyncServiceImplTestAccess::GetImpl(service)->BackgroundSyncStopped();
  }

  static void OnSaveBookmarkOrder(BraveSyncService* service,
                             const std::string& order,
                             const std::string& prev_order,
                             const std::string& next_order,
                             const std::string& parent_order) {
    BraveSyncServiceImplTestAccess::GetImpl(service)->OnSaveBookmarkOrder(
      order, prev_order, next_order, parent_order);
  }

  static void OnResolvedSyncRecords(
    BraveSyncService* service,
    const std::string& category_name,
    std::unique_ptr<RecordsList> records) {
    BraveSyncServiceImplTestAccess::GetImpl(service)->OnResolvedSyncRecords(
      category_name,
      std::move(records));
  }

private:
  static BraveSyncServiceImpl* GetImpl(BraveSyncService* service) {
    return static_cast<BraveSyncServiceImpl*>(service);
  }
};
} // namespace brave_sync

class BraveSyncServiceTest : public testing::Test {
 public:
  BraveSyncServiceTest() {}
  ~BraveSyncServiceTest() override {}

 protected:
  void SetUp() override {
    EXPECT_TRUE(temp_dir_.CreateUniqueTempDir());
    // register the factory

    profile_ = CreateBraveSyncProfile(temp_dir_.GetPath());
    EXPECT_TRUE(profile_.get() != NULL);

    // TODO(bridiver) - this is temporary until some changes are made to
    // to bookmark_change_processor to allow `set_for_testing` like
    // BraveSyncClient
    BookmarkModelFactory::GetInstance()->SetTestingFactory(
       profile(), &BuildFakeBookmarkModelForTests);

    BraveSyncClientImpl::set_for_testing(
        new MockBraveSyncClient());

    sync_service_ = static_cast<BraveSyncServiceImpl*>(
        BraveSyncServiceFactory::GetInstance()->GetForProfile(profile()));

    sync_client_ =
        static_cast<MockBraveSyncClient*>(sync_service_->GetSyncClient());

    observer_.reset(new MockBraveSyncServiceObserver);
    sync_service_->AddObserver(observer_.get());

    DLOG(INFO) << "[Brave Sync Test] service_=" << sync_service_;
    EXPECT_TRUE(sync_service_ != NULL);
  }

  void TearDown() override {
    DLOG(INFO) << "[Brave Sync Test] TearDown()";
    sync_service_->RemoveObserver(observer_.get());
    // this will also trigger a shutdown of the brave sync service
    profile_.reset();
  }

  Profile* profile() { return profile_.get(); }
  BraveSyncServiceImpl* sync_service() { return sync_service_; }
  MockBraveSyncClient* sync_client() { return sync_client_; }
  MockBraveSyncServiceObserver* observer() { return observer_.get(); }

 private:
  // Need this as a very first member to run tests in UI thread
  // When this is set, class should not install any other MessageLoops, like
  // base::test::ScopedTaskEnvironment
  content::TestBrowserThreadBundle thread_bundle_;

  std::unique_ptr<Profile> profile_;
  BraveSyncServiceImpl* sync_service_;
  MockBraveSyncClient* sync_client_;
  std::unique_ptr<MockBraveSyncServiceObserver> observer_;

  base::ScopedTempDir temp_dir_;
};

TEST_F(BraveSyncServiceTest, SetSyncEnabled) {
  EXPECT_CALL(*sync_client(), OnSyncEnabledChanged);
  EXPECT_CALL(*observer(), OnSyncStateChanged(sync_service())).Times(1);
  EXPECT_FALSE(profile()->GetPrefs()->GetBoolean(
      brave_sync::prefs::kSyncEnabled));
  sync_service()->OnSetSyncEnabled(true);
  EXPECT_TRUE(profile()->GetPrefs()->GetBoolean(
      brave_sync::prefs::kSyncEnabled));
  EXPECT_FALSE(sync_service()->IsSyncInitialized());
  EXPECT_FALSE(sync_service()->IsSyncConfigured());
}

TEST_F(BraveSyncServiceTest, SetSyncDisabled) {
  sync_service()->OnSetSyncEnabled(true);
  EXPECT_TRUE(profile()->GetPrefs()->GetBoolean(
      brave_sync::prefs::kSyncEnabled));

  EXPECT_CALL(*sync_client(), OnSyncEnabledChanged).Times(1);
  EXPECT_CALL(*observer(), OnSyncStateChanged(sync_service())).Times(1);
  sync_service()->OnSetSyncEnabled(false);
  EXPECT_FALSE(profile()->GetPrefs()->GetBoolean(
      brave_sync::prefs::kSyncEnabled));
  EXPECT_FALSE(sync_service()->IsSyncInitialized());
  EXPECT_FALSE(sync_service()->IsSyncConfigured());
}

TEST_F(BraveSyncServiceTest, IsSyncConfiguredOnNewProfile) {
  EXPECT_FALSE(sync_service()->IsSyncConfigured());
}

TEST_F(BraveSyncServiceTest, IsSyncInitializedOnNewProfile) {
  EXPECT_FALSE(sync_service()->IsSyncInitialized());
}

TEST_F(BraveSyncServiceTest, BookmarkAdded) {
  DLOG(INFO) << "[Brave Sync Test] TEST_F BookmarkAdded start";

  // BraveSyncService: real
  // BraveSyncClient: mock

  // Invoke BraveSyncService::BookmarkAdded
  // Expect BraveSyncClient::SendGetBookmarkOrder invoked
  // Expect BraveSyncClient::SendSyncRecords invoked

  EXPECT_CALL(*sync_client(), OnSyncEnabledChanged).Times(1);
  EXPECT_CALL(*observer(), OnSyncStateChanged(sync_service())).Times(AtLeast(1));
  sync_service()->OnSetupSyncNewToSync("UnitTestBookmarkAdded");

  DLOG(INFO) << "[Brave Sync Test] firing start loop";
  brave_sync::BraveSyncServiceImplTestAccess::PretendBackgroundSyncStarted(
                                                                sync_service());
  DLOG(INFO) << "[Brave Sync Test] fired start loop";

  auto *bookmark_model = BookmarkModelFactory::GetForBrowserContext(profile());

  EXPECT_CALL(*sync_client(), SendGetBookmarkOrder(_,_,_))
      .Times(AtLeast(1));
  bookmarks::AddIfNotBookmarked(bookmark_model,
                                 GURL("https://a.com"),
                                 base::ASCIIToUTF16("A.com - title"));
  // Emulate answer from client, OnSaveBookmarkOrder - not sure, should it
  // be here as test.
  // BookmarkChangeProcessor::PopRRContext emulates response from the mock
  // Seems wrong, I looked on `BookmarkChangeProcessor::PushRRContext`
  // to catch values.
  // parent order "0" is not quite expected, but enough to get further

  brave_sync::BraveSyncServiceImplTestAccess::OnSaveBookmarkOrder(
    sync_service(), "1.0.4", "", "", "0");

  // Force service send bookmarks and fire the mock
  EXPECT_CALL(*sync_client(), SendSyncRecords(_,_)).Times(1);
  std::unique_ptr<RecordsList> records = std::make_unique<RecordsList>();
  brave_sync::BraveSyncServiceImplTestAccess::OnResolvedSyncRecords(
    sync_service(),
    brave_sync::jslib_const::kBookmarks, std::move(records));

  DLOG(INFO) << "[Brave Sync Test] TEST_F BookmarkAdded done";
}
