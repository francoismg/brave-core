/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <utility>

#include "base/json/json_writer.h"
#include "base/values.h"
#include "bat/ledger/internal/ledger_impl.h"
#include "bat/ledger/internal/request/request_sku.h"
#include "bat/ledger/internal/response/response_sku.h"
#include "bat/ledger/internal/sku/sku_order.h"
#include "bat/ledger/internal/sku/sku_util.h"

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;

namespace braveledger_sku {

SKUOrder::SKUOrder(bat_ledger::LedgerImpl* ledger) :
    ledger_(ledger),
    payment_server_(new ledger::endpoint::PaymentServer(ledger)) {
  DCHECK(ledger_);
}

SKUOrder::~SKUOrder() = default;

void SKUOrder::Create(
    const std::vector<ledger::SKUOrderItem>& items,
    ledger::SKUOrderCallback callback) {
  if (items.empty()) {
    BLOG(0, "List is empty");
    callback(ledger::Result::LEDGER_ERROR, "");
    return;
  }

  auto url_callback = std::bind(&SKUOrder::OnCreate,
      this,
      _1,
      _2,
      callback);

  payment_server_->post_order()->Request(items, url_callback);
}

void SKUOrder::OnCreate(
    const ledger::Result result,
    ledger::SKUOrderPtr order,
    ledger::SKUOrderCallback callback) {
  if (result != ledger::Result::LEDGER_OK) {
    BLOG(0, "Order response could not be parsed");
    callback(ledger::Result::LEDGER_ERROR, "");
    return;
  }

  auto save_callback = std::bind(&SKUOrder::OnCreateSave,
      this,
      _1,
      order->order_id,
      callback);

  ledger_->database()->SaveSKUOrder(order->Clone(), save_callback);
}

void SKUOrder::OnCreateSave(
    const ledger::Result result,
    const std::string& order_id,
    ledger::SKUOrderCallback callback) {
  if (result != ledger::Result::LEDGER_OK) {
    BLOG(0, "Order couldn't be saved");
    callback(result, "");
    return;
  }

  callback(ledger::Result::LEDGER_OK, order_id);
}

}  // namespace braveledger_sku
