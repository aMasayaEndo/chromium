// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sync/test/null_directory_change_delegate.h"

namespace syncable {

NullDirectoryChangeDelegate::~NullDirectoryChangeDelegate() {}

void NullDirectoryChangeDelegate::HandleCalculateChangesChangeEventFromSyncApi(
    const ImmutableWriteTransactionInfo& write_transaction_info,
    BaseTransaction* trans) {}

void NullDirectoryChangeDelegate::HandleCalculateChangesChangeEventFromSyncer(
    const ImmutableWriteTransactionInfo& write_transaction_info,
    BaseTransaction* trans) {}

ModelTypeSet
    NullDirectoryChangeDelegate::HandleTransactionEndingChangeEvent(
        const ImmutableWriteTransactionInfo& write_transaction_info,
        BaseTransaction* trans) {
  return ModelTypeSet();
}

void NullDirectoryChangeDelegate::HandleTransactionCompleteChangeEvent(
    ModelTypeSet models_with_changes) {}

}  // namespace syncable
