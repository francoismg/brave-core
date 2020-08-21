/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/browser/tor/tor_launcher_factory.h"

#include "base/bind.h"
#include "base/process/kill.h"
#include "brave/browser/tor/tor_profile_service_impl.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_launcher_utils.h"
#include "content/public/common/service_manager_connection.h"
#include "services/service_manager/public/cpp/connector.h"

using content::BrowserThread;

namespace {
constexpr char kTorProxyScheme[] = "socks5://";
bool g_prevent_tor_launch_for_tests = false;
}

// static
TorLauncherFactory* TorLauncherFactory::GetInstance() {
  return base::Singleton<TorLauncherFactory>::get();
}

TorLauncherFactory::TorLauncherFactory()
    : is_starting_(false),
      tor_pid_(-1),
      control_(tor::TorControl::Create(this)),
      weak_ptr_factory_(this) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (g_prevent_tor_launch_for_tests) {
    tor_pid_ = 1234;
    VLOG(1) << "Skipping the tor process launch in tests.";
    return;
  }

  Init();
}

void TorLauncherFactory::Init() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  content::ServiceManagerConnection::GetForProcess()->GetConnector()->Connect(
      tor::mojom::kServiceName, tor_launcher_.BindNewPipeAndPassReceiver());

  tor_launcher_.set_disconnect_handler(base::BindOnce(
      &TorLauncherFactory::OnTorLauncherCrashed,
      weak_ptr_factory_.GetWeakPtr()));

  tor_launcher_->SetCrashHandler(base::Bind(
                        &TorLauncherFactory::OnTorCrashed,
                        weak_ptr_factory_.GetWeakPtr()));
}

TorLauncherFactory::~TorLauncherFactory() {}

bool TorLauncherFactory::SetConfig(const tor::TorConfig& config) {
  if (config.empty())
    return false;
  config_ = config;
  return true;
}

void TorLauncherFactory::LaunchTorProcess(const tor::TorConfig& config) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (g_prevent_tor_launch_for_tests) {
    VLOG(1) << "Skipping the tor process launch in tests.";
    return;
  }

  if (is_starting_) {
    LOG(WARNING) << "tor process is already starting";
    return;
  } else {
    is_starting_ = true;
  }

  if (tor_pid_ >= 0) {
    LOG(WARNING) << "tor process(" << tor_pid_ << ") is running";
    return;
  }
  if (!SetConfig(config)) {
    LOG(WARNING) << "config is empty";
    return;
  }

  // Tor launcher could be null if we created Tor process and killed it
  // through KillTorProcess function before. So we need to initialize
  // tor_launcher_ again here.
  if (!tor_launcher_) {
    Init();
  }

  // Launch tor after cleanup is done
  control_->Start(config_.tor_watch_path(),
                  base::BindOnce(&TorLauncherFactory::OnTorControlCheckComplete,
                                 weak_ptr_factory_.GetWeakPtr()));
}

void TorLauncherFactory::OnTorControlCheckComplete() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  tor_launcher_->Launch(config_,
                        base::Bind(&TorLauncherFactory::OnTorLaunched,
                                   weak_ptr_factory_.GetWeakPtr()));
}

void TorLauncherFactory::KillTorProcess() {
  control_->Stop();
  tor_launcher_.reset();
  tor_pid_ = -1;
}

void TorLauncherFactory::AddObserver(tor::TorProfileServiceImpl* service) {
  observers_.AddObserver(service);
}

void TorLauncherFactory::RemoveObserver(tor::TorProfileServiceImpl* service) {
  observers_.RemoveObserver(service);
}

void TorLauncherFactory::OnTorLauncherCrashed() {
  LOG(ERROR) << "Tor Launcher Crashed";
  is_starting_ = false;
  for (auto& observer : observers_)
    observer.NotifyTorLauncherCrashed();
}

void TorLauncherFactory::OnTorCrashed(int64_t pid) {
  LOG(ERROR) << "Tor Process(" << pid << ") Crashed";
  is_starting_ = false;
  for (auto& observer : observers_)
    observer.NotifyTorCrashed(pid);
}

void TorLauncherFactory::OnTorLaunched(bool result, int64_t pid) {
  if (result) {
    is_starting_ = false;
    tor_pid_ = pid;
  } else {
    LOG(ERROR) << "Tor Launching Failed(" << pid <<")";
  }
  for (auto& observer : observers_)
    observer.NotifyTorLaunched(result, pid);
}

void TorLauncherFactory::OnTorControlReady() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  LOG(ERROR) << "TOR CONTROL: Ready!";
  control_->GetVersion(
      base::BindOnce(&TorLauncherFactory::GotVersion,
                     weak_ptr_factory_.GetWeakPtr()));
  control_->GetSOCKSListeners(
      base::BindOnce(&TorLauncherFactory::GotSOCKSListeners,
                     weak_ptr_factory_.GetWeakPtr()));
  control_->Subscribe(tor::TorControlEvent::NETWORK_LIVENESS,
                      base::DoNothing::Once<bool>());
  control_->Subscribe(tor::TorControlEvent::STATUS_CLIENT,
                      base::DoNothing::Once<bool>());
  control_->Subscribe(tor::TorControlEvent::STATUS_GENERAL,
                      base::DoNothing::Once<bool>());
#if 0
  control_->Subscribe(tor::TorControlEvent::STREAM,
                      base::DoNothing::Once<bool>());
#endif
}

void TorLauncherFactory::GotVersion(bool error, const std::string& version) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (error) {
    LOG(ERROR) << "Failed to get version!";
    return;
  }
  LOG(ERROR) << "Tor version: " << version;
}

void TorLauncherFactory::GotSOCKSListeners(
    bool error, const std::vector<std::string>& listeners) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (error) {
    LOG(ERROR) << "Failed to get SOCKS listeners!";
    return;
  }
  LOG(ERROR) << "Tor SOCKS listeners: ";
  for (auto& listener : listeners) {
    LOG(ERROR) << listener;
  }
  std::string tor_proxy_uri = kTorProxyScheme + listeners[0];
  // Remove extra quotes
  tor_proxy_uri.erase(
      std::remove(tor_proxy_uri.begin(), tor_proxy_uri.end(), '\"'),
      tor_proxy_uri.end());
  for (auto& observer : observers_)
    observer.NotifyTorNewProxyURI(tor_proxy_uri);
}

void TorLauncherFactory::OnTorClosed() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  LOG(ERROR) << "TOR CONTROL: Closed!";
}

void TorLauncherFactory::OnTorCleanupNeeded(base::ProcessId id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  LOG(ERROR) << "Killing old tor process pid=" << id;
  // Dispatch to launcher thread
  content::GetProcessLauncherTaskRunner()
    ->PostTask(FROM_HERE,
               base::BindOnce(&TorLauncherFactory::KillOldTorProcess,
                              base::Unretained(this),
                              std::move(id)));
}


void TorLauncherFactory::KillOldTorProcess(base::ProcessId id) {
  DCHECK(content::CurrentlyOnProcessLauncherTaskRunner());
  base::Process tor_process = base::Process::Open(id);
  tor_process.Terminate(0, false);
}

void TorLauncherFactory::OnTorEvent(
    tor::TorControlEvent event, const std::string& initial,
    const std::map<std::string, std::string>& extra) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  LOG(ERROR) << "TOR CONTROL: event "
             << (*tor::kTorControlEventByEnum.find(event)).second
             << ": " << initial;
  if (event == tor::TorControlEvent::STATUS_CLIENT) {
    if (initial.find("BOOTSTRAP") != std::string::npos) {
      const char prefix[] = "PROGRESS=";
      size_t progress_start = initial.find(prefix);
      size_t progress_length = initial.substr(progress_start).find(" ") ;
      // Dispatch progress
      const std::string percentage =
        initial.substr(progress_start + strlen(prefix),
                       progress_length - strlen(prefix));
      for (auto& observer : observers_)
        observer.NotifyTorInitializing(percentage);
    } else if (initial.find("CIRCUIT_ESTABLISHED") != std::string::npos) {
      for (auto& observer : observers_)
        observer.NotifyTorCircuitEstablished(true);
    } else if (initial.find("CIRCUIT_NOT_ESTABLISHED") != std::string::npos) {
      for (auto& observer : observers_)
        observer.NotifyTorCircuitEstablished(false);
    }
  }
}

void TorLauncherFactory::OnTorRawCmd(const std::string& cmd) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  LOG(ERROR) << "TOR CONTROL: command: " << cmd;
}

void TorLauncherFactory::OnTorRawAsync(
    const std::string& status, const std::string& line) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  LOG(ERROR) << "TOR CONTROL: async " << status << " " << line;
}

void TorLauncherFactory::OnTorRawMid(
    const std::string& status, const std::string& line) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  LOG(ERROR) << "TOR CONTROL: mid " << status << "-" << line;
}

void TorLauncherFactory::OnTorRawEnd(
    const std::string& status, const std::string& line) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  LOG(ERROR) << "TOR CONTROL: end " << status << " " << line;
}

ScopedTorLaunchPreventerForTest::ScopedTorLaunchPreventerForTest() {
  g_prevent_tor_launch_for_tests = true;
}

ScopedTorLaunchPreventerForTest::~ScopedTorLaunchPreventerForTest() {
  g_prevent_tor_launch_for_tests = false;
}
