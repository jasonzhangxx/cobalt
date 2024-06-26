// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "components/update_client/task_send_uninstall_ping.h"

#include <utility>

#include "base/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/version.h"
#include "components/update_client/update_client.h"
#include "components/update_client/update_engine.h"

namespace update_client {

TaskSendUninstallPing::TaskSendUninstallPing(
    scoped_refptr<UpdateEngine> update_engine,
    const std::string& id,
    const base::Version& version,
    int reason,
    Callback callback)
    : update_engine_(update_engine),
      id_(id),
      version_(version),
      reason_(reason),
      callback_(std::move(callback)) {}

TaskSendUninstallPing::~TaskSendUninstallPing() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void TaskSendUninstallPing::Run() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (id_.empty()) {
    TaskComplete(Error::INVALID_ARGUMENT);
    return;
  }

  update_engine_->SendUninstallPing(
      id_, version_, reason_,
      base::BindOnce(&TaskSendUninstallPing::TaskComplete, this));
}

void TaskSendUninstallPing::Cancel() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  TaskComplete(Error::UPDATE_CANCELED);
}

std::vector<std::string> TaskSendUninstallPing::GetIds() const {
  return std::vector<std::string>{id_};
}

void TaskSendUninstallPing::TaskComplete(Error error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback_), scoped_refptr<Task>(this), error));
}

}  // namespace update_client
