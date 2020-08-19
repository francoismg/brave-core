// Copyright (c) 2020 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// you can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef BRAVE_CHROMIUM_SRC_COMPONENTS_SYNC_DEVICE_INFO_DEVICE_INFO_PREFS_H_
#define BRAVE_CHROMIUM_SRC_COMPONENTS_SYNC_DEVICE_INFO_DEVICE_INFO_PREFS_H_

class PrefService;

#define GarbageCollectExpiredCacheGuids   \
    GarbageCollectExpiredCacheGuids();     \
    PrefService* GetPrefService

#include "../../../../components/sync_device_info/device_info_prefs.h"

#undef GarbageCollectExpiredCacheGuids
#endif  // BRAVE_CHROMIUM_SRC_COMPONENTS_SYNC_DEVICE_INFO_DEVICE_INFO_PREFS_H_
