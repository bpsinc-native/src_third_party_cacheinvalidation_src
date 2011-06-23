// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Class to schedule future operations such that if one has already been
// scheduled for the same operation, another one is not scheduled.

#include "google/cacheinvalidation/v2/operation-scheduler.h"

#include "google/cacheinvalidation/v2/logging.h"
#include "google/cacheinvalidation/v2/log-macro.h"

namespace invalidation {

void OperationScheduler::SetOperation(TimeDelta delay, Closure* operation) {
  CHECK(operations_.find(operation) != operations_.end()) << "operation " <<
      operation << " already set";
  CHECK(delay > TimeDelta::FromMilliseconds(0)) <<
      "delay_ms must be positive: given " << delay.ToInternalValue();
  CHECK(operation != NULL);
  TLOG(logger_, FINE, "Set %llx with delay %d", operation,
       delay.ToInternalValue());
  operations_[operation] = OperationScheduleInfo(delay);
}

void OperationScheduler::ChangeDelayForTest(
    Closure* operation, TimeDelta delay) {
  hash_map<Closure*, OperationScheduleInfo, ClosureHashFunction>::iterator it =
      operations_.find(operation);
  CHECK(it != operations_.end());
  OperationScheduleInfo& op_info = it->second;
  TLOG(logger_, FINE, "Changing delay for %llx to be %d ms", operation,
       delay.ToInternalValue());
  op_info.delay = delay;
}

void OperationScheduler::Schedule(Closure* operation) {
  hash_map<Closure*, OperationScheduleInfo, ClosureHashFunction>::iterator it =
      operations_.find(operation);
  CHECK(it != operations_.end());
  OperationScheduleInfo* op_info = &it->second;

  // Schedule an event if one has not been already scheduled.
  if (!op_info->has_been_scheduled) {
    TLOG(logger_, FINE, "Scheduling %llx with a delay %d, Now = %s", operation,
         op_info->delay.ToInternalValue(),
         scheduler_->GetCurrentTime().ToInternalValue());
    op_info->has_been_scheduled = true;
    scheduler_->Schedule(
        op_info->delay,
        NewPermanentCallback(
            &OperationScheduler::RunAndClearScheduled, operation, op_info));
  }
}

void OperationScheduler::RunAndClearScheduled(
    Closure* closure, OperationScheduleInfo* info) {
  closure->Run();
  info->has_been_scheduled = false;
}

}  // namespace invalidation
