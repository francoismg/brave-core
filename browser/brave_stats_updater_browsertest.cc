/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/environment.h"
#include "base/path_service.h"
#include "base/time/time.h"
#include "brave/browser/brave_referrals/brave_referrals_service_factory.h"
#include "brave/browser/brave_stats_updater.h"
#include "brave/browser/brave_stats_updater_params.h"
#include "brave/common/pref_names.h"
#include "brave/components/brave_referrals/browser/brave_referrals_service.h"
#include "brave/components/brave_referrals/common/pref_names.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/testing_pref_service.h"
#include "content/public/test/browser_test.h"
#include "net/base/url_util.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"

namespace {

// Request handler for stats and referral updates. The response this returns
// doesn't represent a valid update server response, but it's sufficient for
// testing purposes as we're not interested in the contents of the
// response.
std::unique_ptr<net::test_server::HttpResponse> HandleRequestForStats(
    const net::test_server::HttpRequest& request) {

  std::unique_ptr<net::test_server::BasicHttpResponse> http_response(
      new net::test_server::BasicHttpResponse());
  if (request.relative_url == "/promo/initialize/nonua") {
    // We need a download id to make promo initialization happy
    http_response->set_code(net::HTTP_OK);
    http_response->set_content("{\"download_id\":\"keur123\"}");
    http_response->set_content_type("application/json");
  } else {
    http_response->set_code(net::HTTP_OK);
    http_response->set_content_type("text/html");
    http_response->set_content("<html><head></head></html>");
  }
  return std::move(http_response);
}

}  // anonymous namespace

class BraveStatsUpdaterBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    brave::RegisterPrefsForBraveStatsUpdater(testing_local_state_.registry());
    brave::RegisterPrefsForBraveReferralsService(
        testing_local_state_.registry());
    InitEmbeddedTestServer();
    SetBaseUpdateURLForTest();
    // Simulate sentinel file creation as if chrome_browser_main.h was called,
    // which reads in the sentinel value and caches it.
    brave::BraveStatsUpdaterParams::SetFirstRunForTest(true);
  }

  void InitEmbeddedTestServer() {
    embedded_test_server()->RegisterRequestHandler(
        base::Bind(&HandleRequestForStats));
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  void SetBaseUpdateURLForTest() {
    std::unique_ptr<base::Environment> env(base::Environment::Create());
    brave::BraveStatsUpdater::SetBaseUpdateURLForTest(
        embedded_test_server()->GetURL("/1/usage/brave-core").spec());
    env->SetVar("BRAVE_REFERRALS_SERVER",
        embedded_test_server()->host_port_pair().ToString());
    env->SetVar("BRAVE_REFERRALS_LOCAL", "1");  // use http for local testing
  }

  PrefService* GetLocalState() { return &testing_local_state_; }

  std::string GetUpdateURL() const { return update_url_; }

  int WritePromoCodeFile(const std::string& referral_code) {
    base::ScopedAllowBlockingForTesting allow_blocking;

    base::FilePath user_data_dir;
    base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir);

    const base::FilePath promo_code_file =
        user_data_dir.AppendASCII("promoCode");
    return base::WriteFile(promo_code_file, referral_code.c_str(),
                           referral_code.size());
  }

  void OnReferralInitialized(const std::string& download_id) {
    referral_was_initialized_ = true;
    // TODO(keur): Maybe do something with the download id
    wait_for_callback_loop_->Quit();
  }

  void WaitForReferralInitializeCallback() {
    if (referral_was_initialized_)
      return;
    wait_for_callback_loop_.reset(new base::RunLoop);
    wait_for_callback_loop_->Run();
  }

  void OnStatsUpdated(const std::string& update_url) {
    stats_was_called_ = true;
    update_url_ = update_url;
    wait_for_callback_loop_->Quit();
  }

  void WaitForStatsUpdatedCallback() {
    if (stats_was_called_)
      return;
    wait_for_callback_loop_.reset(new base::RunLoop);
    wait_for_callback_loop_->Run();
  }

 private:
  TestingPrefServiceSimple testing_local_state_;
  std::unique_ptr<base::RunLoop> wait_for_callback_loop_;
  bool stats_was_called_ = false;
  bool referral_was_initialized_ = false;
  std::string update_url_;
};

// Run the stats updater and verify that it sets the first check preference
IN_PROC_BROWSER_TEST_F(BraveStatsUpdaterBrowserTest,
                       StatsUpdaterSetsFirstCheckPreference) {
  // Ensure that first check preference is false
  ASSERT_FALSE(GetLocalState()->GetBoolean(kFirstCheckMade));

  // Start the referrals service, since the stats updater's startup
  // ping only occurs after the referrals service checks for the promo
  // code file
  auto referrals_service = brave::BraveReferralsServiceFactory::GetInstance()
    ->GetForPrefs(GetLocalState());
  referrals_service->SetReferralInitializedCallbackForTest(base::BindRepeating(
      &BraveStatsUpdaterBrowserTest::OnReferralInitialized,
      base::Unretained(this)));
  referrals_service->Start();
  WaitForReferralInitializeCallback();

  // Start the stats updater, wait for it to perform its startup ping,
  // and then shut it down
  brave::BraveStatsUpdater stats_updater(GetLocalState());
  stats_updater.SetStatsUpdatedCallback(base::BindRepeating(
      &BraveStatsUpdaterBrowserTest::OnStatsUpdated, base::Unretained(this)));
  stats_updater.Start();
  WaitForStatsUpdatedCallback();
  stats_updater.Stop();

  // Stop the referrals service
  referrals_service->Stop();

  // First check preference should now be true
  EXPECT_TRUE(GetLocalState()->GetBoolean(kFirstCheckMade));
}

// Run the stats updater with no active referral and verify that the
// update url specifies the default referral code
IN_PROC_BROWSER_TEST_F(BraveStatsUpdaterBrowserTest,
                       StatsUpdaterStartupPingWithDefaultReferralCode) {
  // Ensure that checked for promo code file preference is false
  ASSERT_FALSE(GetLocalState()->GetBoolean(kReferralInitialization));

  // Start the referrals service, since the stats updater's startup
  // ping only occurs after the referrals service checks for the promo
  // code file
  auto referrals_service = brave::BraveReferralsServiceFactory::GetInstance()
    ->GetForPrefs(GetLocalState());
  referrals_service->SetReferralInitializedCallbackForTest(base::BindRepeating(
      &BraveStatsUpdaterBrowserTest::OnReferralInitialized,
      base::Unretained(this)));
  referrals_service->Start();
  WaitForReferralInitializeCallback();

  // Start the stats updater, wait for it to perform its startup ping,
  // and then shut it down
  brave::BraveStatsUpdater stats_updater(GetLocalState());
  stats_updater.SetStatsUpdatedCallback(base::BindRepeating(
      &BraveStatsUpdaterBrowserTest::OnStatsUpdated, base::Unretained(this)));
  stats_updater.Start();
  WaitForStatsUpdatedCallback();
  stats_updater.Stop();

  // Stop the referrals service
  referrals_service->Stop();

  // Promo code file preference should now be true
  EXPECT_TRUE(GetLocalState()->GetBoolean(kReferralInitialization));

  // Verify that update url is valid
  const GURL update_url(GetUpdateURL());
  EXPECT_TRUE(update_url.is_valid());

  // Verify that daily parameter is true
  std::string query_value;
  EXPECT_TRUE(net::GetValueForKeyInQuery(update_url, "daily", &query_value));
  EXPECT_STREQ(query_value.c_str(), "true");

  // Verify that there is no referral code
  EXPECT_TRUE(net::GetValueForKeyInQuery(update_url, "ref", &query_value));
  EXPECT_STREQ(query_value.c_str(), "BRV001");
}

IN_PROC_BROWSER_TEST_F(BraveStatsUpdaterBrowserTest,
                       StatsUpdaterMigration) {
  // Create a pre 1.19 user.
  // Has a download_id, kReferralCheckedForPromoCodeFile is set, has promo code.
  ASSERT_FALSE(GetLocalState()->GetBoolean(kReferralInitialization));
  GetLocalState()->SetString(kReferralDownloadID, "migration");
  GetLocalState()->SetString(kReferralPromoCode, "BRV001");
  GetLocalState()->SetBoolean(kReferralCheckedForPromoCodeFile, true);

  // Start the referrals service, since the stats updater's startup
  // ping only occurs after the referrals service checks for the promo
  // code file
  auto referrals_service = brave::BraveReferralsServiceFactory::GetInstance()
    ->GetForPrefs(GetLocalState());
  referrals_service->SetReferralInitializedCallbackForTest(base::BindRepeating(
      &BraveStatsUpdaterBrowserTest::OnReferralInitialized,
      base::Unretained(this)));
  referrals_service->Start();
  // NOTE: Don't call WaitForReferralInitializeCallback(); since a user
  // migrating from an earlier version is aleady initialized, and that will
  // never trigger.

  // Start the stats updater, wait for it to perform its startup ping,
  // and then shut it down
  brave::BraveStatsUpdater stats_updater(GetLocalState());
  stats_updater.SetStatsUpdatedCallback(base::BindRepeating(
      &BraveStatsUpdaterBrowserTest::OnStatsUpdated, base::Unretained(this)));
  stats_updater.Start();
  WaitForStatsUpdatedCallback();
  stats_updater.Stop();

  // Stop the referrals service
  referrals_service->Stop();

  // Verify that update url is valid
  const GURL update_url(GetUpdateURL());
  EXPECT_TRUE(update_url.is_valid());

  // Verify that daily parameter is true
  std::string query_value;
  EXPECT_TRUE(net::GetValueForKeyInQuery(update_url, "daily", &query_value));
  EXPECT_STREQ(query_value.c_str(), "true");

  // Verify that there is no referral code
  EXPECT_TRUE(net::GetValueForKeyInQuery(update_url, "ref", &query_value));
  EXPECT_STREQ(query_value.c_str(), "BRV001");
}

// Run the stats updater with an active referral and verify that the
// update url includes the referral code
IN_PROC_BROWSER_TEST_F(BraveStatsUpdaterBrowserTest,
                       StatsUpdaterStartupPingWithReferralCode) {
  // Ensure that checked for promo code file preference is false
  ASSERT_FALSE(GetLocalState()->GetBoolean(kReferralInitialization));

  // Write the promo code file out to the user data directory
  const std::string referral_code = "FOO123";
  ASSERT_EQ(WritePromoCodeFile(referral_code),
            base::checked_cast<int>(referral_code.size()));

  // Start the referrals service, since the stats updater's startup
  // ping only occurs after the referrals service checks for the promo
  // code file
  auto referrals_service = brave::BraveReferralsServiceFactory::GetInstance()
    ->GetForPrefs(GetLocalState());
  referrals_service->SetReferralInitializedCallbackForTest(base::BindRepeating(
      &BraveStatsUpdaterBrowserTest::OnReferralInitialized,
      base::Unretained(this)));
  referrals_service->Start();
  WaitForReferralInitializeCallback();

  // Start the stats updater, wait for it to perform its startup ping,
  // and then shut it down
  brave::BraveStatsUpdater stats_updater(GetLocalState());
  stats_updater.SetStatsUpdatedCallback(base::BindRepeating(
      &BraveStatsUpdaterBrowserTest::OnStatsUpdated, base::Unretained(this)));
  stats_updater.Start();
  WaitForStatsUpdatedCallback();
  stats_updater.Stop();

  // Stop the referrals service
  referrals_service->Stop();

  // Promo code file preference should now be true
  EXPECT_TRUE(GetLocalState()->GetBoolean(kReferralInitialization));

  // Verify that update url is valid
  const GURL update_url(GetUpdateURL());
  EXPECT_TRUE(update_url.is_valid());

  // Verify that daily parameter is true
  std::string query_value;
  EXPECT_TRUE(net::GetValueForKeyInQuery(update_url, "daily", &query_value));
  EXPECT_STREQ(query_value.c_str(), "true");

  // Verify that the expected referral code is present
  EXPECT_TRUE(net::GetValueForKeyInQuery(update_url, "ref", &query_value));
  EXPECT_STREQ(query_value.c_str(), referral_code.c_str());
}
