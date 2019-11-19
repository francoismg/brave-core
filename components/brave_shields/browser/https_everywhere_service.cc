/* Copyright 2016 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_shields/browser/https_everywhere_service.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/threading/scoped_blocking_call.h"
#include "brave/vendor/https-everywhere-lib-cpp/src/wrapper.hpp"

#define JSON_FILE "httpse.json"
#define JSON_FILE_VERSION "1"
#define HTTPSE_URLS_REDIRECTS_COUNT_QUEUE   1
#define HTTPSE_URL_MAX_REDIRECTS_COUNT      5

namespace brave_shields {

const char kHTTPSEverywhereComponentName[] = "Brave HTTPS Everywhere Updater";
const char kHTTPSEverywhereComponentId[] = "oofiananboodjbbmdelgdommihjbkfag";
const char kHTTPSEverywhereComponentBase64PublicKey[] =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAvn9zSMjTmhkQyrZu5UdN"
    "350nPqLoSeCYngcC7yDFwaUHjoBQXCZqGeDC69ciCQ2mlRhcV2nxXqlUDkiC6+7m"
    "651nI+gi4oVqHagc7EFUyGA0yuIk7qIMvCBdH7wbET27de0rzbRzRht9EKzEjIhC"
    "BtoPnmyrO/8qPrH4XR4cPfnFPuJssBBxC1B35H7rh0Br9qePhPDDe9OjyqYxPuio"
    "+YcC9obL4g5krVrfrlKLfFNpIewUcJyBpSlCgfxEyEhgDkK9cILTMUi5vC7GxS3P"
    "OtZqgfRg8Da4i+NwmjQqrz0JFtPMMSyUnmeMj+mSOL4xZVWr8fU2/GOCXs9gczDp"
    "JwIDAQAB";

bool HTTPSEverywhereService::g_ignore_port_for_test_(false);
std::string HTTPSEverywhereService::g_https_everywhere_component_id_(
    kHTTPSEverywhereComponentId);
std::string
HTTPSEverywhereService::g_https_everywhere_component_base64_public_key_(
    kHTTPSEverywhereComponentBase64PublicKey);

HTTPSEverywhereService::HTTPSEverywhereService(
    BraveComponent::Delegate* delegate)
    : BaseBraveShieldsService(delegate),
      rust_client_(httpse::HttpsEverywhereClient()) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

HTTPSEverywhereService::~HTTPSEverywhereService() {
}

bool HTTPSEverywhereService::Init() {
  Register(kHTTPSEverywhereComponentName,
           g_https_everywhere_component_id_,
           g_https_everywhere_component_base64_public_key_);
  return true;
}

void HTTPSEverywhereService::InitDB(const base::FilePath& install_dir) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::FilePath json_ruleset_filepath =
      install_dir.AppendASCII(JSON_FILE_VERSION).AppendASCII(JSON_FILE);
  std::string rules;
  base::ReadFileToString(json_ruleset_filepath, &rules);
  rust_client_.load_rules(rules);
}

void HTTPSEverywhereService::OnComponentReady(
    const std::string& component_id,
    const base::FilePath& install_dir,
    const std::string& manifest) {
  GetTaskRunner()->PostTask(
      FROM_HERE,
      base::Bind(&HTTPSEverywhereService::InitDB,
                 AsWeakPtr(),
                 install_dir));
}

bool HTTPSEverywhereService::GetHTTPSURL(
    const GURL* url,
    const uint64_t& request_identifier,
    std::string* new_url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!url->is_valid())
    return false;

  if (!IsInitialized() || url->scheme() == url::kHttpsScheme) {
    return false;
  }
  if (!ShouldHTTPSERedirect(request_identifier)) {
    return false;
  }

  if (recently_used_cache_.get(url->spec(), new_url)) {
    AddHTTPSEUrlToRedirectList(request_identifier);
    return true;
  }

  GURL candidate_url(*url);
  if (g_ignore_port_for_test_ && candidate_url.has_port()) {
    GURL::Replacements replacements;
    replacements.ClearPort();
    candidate_url = candidate_url.ReplaceComponents(replacements);
  }

  auto rewrite_result = rust_client_.rewrite_url(candidate_url.spec());
  if (rewrite_result.action == C_RewriteActionEnum::RewriteUrl) {
    recently_used_cache_.add(candidate_url.spec(), rewrite_result.new_url);
    AddHTTPSEUrlToRedirectList(request_identifier);
    return true;
  }
  recently_used_cache_.remove(candidate_url.spec());
  return false;
}

bool HTTPSEverywhereService::GetHTTPSURLFromCacheOnly(
    const GURL* url,
    const uint64_t& request_identifier,
    std::string* cached_url) {
  if (!url->is_valid())
    return false;

  if (!IsInitialized() || url->scheme() == url::kHttpsScheme) {
    return false;
  }
  if (!ShouldHTTPSERedirect(request_identifier)) {
    return false;
  }

  if (recently_used_cache_.get(url->spec(), cached_url)) {
    AddHTTPSEUrlToRedirectList(request_identifier);
    return true;
  }
  return false;
}

bool HTTPSEverywhereService::ShouldHTTPSERedirect(
    const uint64_t& request_identifier) {
  base::AutoLock auto_lock(httpse_get_urls_redirects_count_mutex_);
  for (size_t i = 0; i < httpse_urls_redirects_count_.size(); i++) {
    if (request_identifier ==
        httpse_urls_redirects_count_[i].request_identifier_ &&
        httpse_urls_redirects_count_[i].redirects_ >=
        HTTPSE_URL_MAX_REDIRECTS_COUNT - 1) {
      return false;
    }
  }

  return true;
}

void HTTPSEverywhereService::AddHTTPSEUrlToRedirectList(
    const uint64_t& request_identifier) {
  // Adding redirects count for the current request
  base::AutoLock auto_lock(httpse_get_urls_redirects_count_mutex_);
  bool hostFound = false;
  for (size_t i = 0; i < httpse_urls_redirects_count_.size(); i++) {
    if (request_identifier ==
        httpse_urls_redirects_count_[i].request_identifier_) {
      // Found the host, just increment the redirects_count
      httpse_urls_redirects_count_[i].redirects_++;
      hostFound = true;
      break;
    }
  }
  if (!hostFound) {
      // The host is new, adding it to the redirects list
      if (httpse_urls_redirects_count_.size() >=
          HTTPSE_URLS_REDIRECTS_COUNT_QUEUE) {
          // The queue is full, erase the first element
          httpse_urls_redirects_count_.erase(
              httpse_urls_redirects_count_.begin());
      }
      httpse_urls_redirects_count_.push_back(
          HTTPSE_REDIRECTS_COUNT_ST(request_identifier, 1));
  }
}

void HTTPSEverywhereService::CloseDatabase() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

// static
void HTTPSEverywhereService::SetComponentIdAndBase64PublicKeyForTest(
    const std::string& component_id,
    const std::string& component_base64_public_key) {
  g_https_everywhere_component_id_ = component_id;
  g_https_everywhere_component_base64_public_key_ = component_base64_public_key;
}

// static
void HTTPSEverywhereService::SetIgnorePortForTest(bool ignore) {
  g_ignore_port_for_test_ = ignore;
}

///////////////////////////////////////////////////////////////////////////////

// The brave shields factory. Using the Brave Shields as a singleton
// is the job of the browser process.
std::unique_ptr<HTTPSEverywhereService> HTTPSEverywhereServiceFactory(
    BraveComponent::Delegate* delegate) {
  return std::make_unique<HTTPSEverywhereService>(delegate);
}

}  // namespace brave_shields
