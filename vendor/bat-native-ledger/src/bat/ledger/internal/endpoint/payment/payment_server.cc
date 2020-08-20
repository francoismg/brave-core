/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat/ledger/internal/endpoint/payment/payment_server.h"

#include "bat/ledger/internal/ledger_impl.h"

namespace ledger {
namespace endpoint {

PaymentServer::PaymentServer(bat_ledger::LedgerImpl* ledger):
    ledger_(ledger),
    post_order_(new payment::PostOrder(ledger)) {
  DCHECK(ledger_);
}

PaymentServer::~PaymentServer() = default;

payment::PostOrder* PaymentServer::post_order() const {
  return post_order_.get();
}

}  // namespace endpoint
}  // namespace ledger
