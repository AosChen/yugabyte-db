// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/util/mem_tracker.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#include <boost/range/algorithm_ext/erase.hpp>

#include "yb/gutil/map-util.h"
#include "yb/gutil/once.h"
#include "yb/gutil/strings/human_readable.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/util/debug-util.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/env.h"
#include "yb/util/flags.h"
#include "yb/util/format.h"
#include "yb/util/memory/memory.h"
#include "yb/util/metrics.h"
#include "yb/util/mutex.h"
#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"
#include "yb/util/status.h"
#include "yb/util/status_log.h"
#include "yb/util/logging.h"
#include "yb/util/tcmalloc_trace.h"
#include "yb/util/tcmalloc_util.h"
#include "yb/util/tcmalloc_impl_util.h"

using namespace std::literals;

DEFINE_UNKNOWN_int64(memory_limit_hard_bytes, 0,
             "Maximum amount of memory this daemon should use, in bytes. "
             "A value of 0 autosizes based on the total system memory. "
             "A value of -1 disables all memory limiting.");
TAG_FLAG(memory_limit_hard_bytes, stable);
DEFINE_NON_RUNTIME_double(default_memory_limit_to_ram_ratio, 0.85,
              "If memory_limit_hard_bytes is left unspecified, then it is "
              "set to default_memory_limit_to_ram_ratio * Available RAM.");
TAG_FLAG(default_memory_limit_to_ram_ratio, advanced);

DEFINE_UNKNOWN_int32(memory_limit_soft_percentage, 85,
             "Percentage of the hard memory limit that this daemon may "
             "consume before memory throttling of writes begins. The greater "
             "the excess, the higher the chance of throttling. In general, a "
             "lower soft limit leads to smoother write latencies but "
             "decreased throughput, and vice versa for a higher soft limit.");
TAG_FLAG(memory_limit_soft_percentage, advanced);

DEFINE_UNKNOWN_int32(memory_limit_warn_threshold_percentage, 98,
             "Percentage of the hard memory limit that this daemon may "
             "consume before WARNING level messages are periodically logged.");
TAG_FLAG(memory_limit_warn_threshold_percentage, advanced);

DEFINE_UNKNOWN_int32(tcmalloc_max_free_bytes_percentage, 10,
                     "Maximum percentage of the RSS that tcmalloc is allowed to use for "
                     "reserved but unallocated memory.");
TAG_FLAG(tcmalloc_max_free_bytes_percentage, advanced);

DEFINE_UNKNOWN_bool(mem_tracker_logging, false,
            "Enable logging of memory tracker consume/release operations");

DEFINE_UNKNOWN_bool(mem_tracker_log_stack_trace, false,
            "Enable logging of stack traces on memory tracker consume/release operations. "
            "Only takes effect if mem_tracker_logging is also enabled.");

DEFINE_NON_RUNTIME_int64(mem_tracker_update_consumption_interval_us, 2 * 1000 * 1000,
    "Interval that is used to update memory consumption from external source. "
    "For instance from tcmalloc statistics.");

DEFINE_RUNTIME_int64(mem_tracker_tcmalloc_gc_release_bytes, -1,
    "When the total amount of memory from calls to Release() since the last GC exceeds "
    "this flag, a new tcmalloc GC will be triggered. This GC will clear the tcmalloc "
    "page heap freelist. A higher value implies less aggressive GC, i.e. higher memory "
    "overhead, but more efficient in terms of runtime.");

DECLARE_int64(server_tcmalloc_max_total_thread_cache_bytes);

namespace yb {

// NOTE: this class has been adapted from Impala, so the code style varies
// somewhat from yb.

using std::string;
using std::stringstream;
using std::shared_ptr;
using std::vector;

using strings::Substitute;

namespace {

// The ancestor for all trackers. Every tracker is visible from the root down.
shared_ptr<MemTracker> root_tracker;
GoogleOnceType root_tracker_once = GOOGLE_ONCE_INIT;

// Total amount of memory from calls to Release() since the last GC. If this
// is greater than mem_tracker_tcmalloc_gc_release_bytes, this will trigger a tcmalloc gc.
Atomic64 released_memory_since_gc;

// Marked as unused because this is not referenced in release mode.
DEFINE_validator(memory_limit_soft_percentage, &::yb::ValidatePercentageFlag);
DEFINE_validator(memory_limit_warn_threshold_percentage, &::yb::ValidatePercentageFlag);

template <class TrackerMetrics>
bool TryIncrementBy(
    int64_t delta, int64_t max, HighWaterMark* consumption,
    const std::unique_ptr<TrackerMetrics>& metrics) {
  if (consumption->TryIncrementBy(delta, max)) {
    if (metrics) {
      metrics->metric_->IncrementBy(delta);
    }
    return true;
  }
  return false;
}

template <class TrackerMetrics>
void IncrementBy(int64_t amount, HighWaterMark* consumption,
                 const std::unique_ptr<TrackerMetrics>& metrics) {
  consumption->IncrementBy(amount);
  if (metrics) {
    metrics->metric_->IncrementBy(amount);
  }
}

std::string CreateMetricName(const MemTracker& mem_tracker) {
  if (mem_tracker.metric_entity() &&
        (!mem_tracker.parent() ||
            mem_tracker.parent()->metric_entity().get() != mem_tracker.metric_entity().get())) {
    return "mem_tracker";
  }
  std::string id = mem_tracker.id();
  EscapeMetricNameForPrometheus(&id);
  if (mem_tracker.parent()) {
    return CreateMetricName(*mem_tracker.parent()) + "_" + id;
  } else {
    return id;
  }
}

std::string CreateMetricLabel(const MemTracker& mem_tracker) {
  return Format("Memory consumed by $0", mem_tracker.ToString());
}

std::string CreateMetricDescription(const MemTracker& mem_tracker) {
  return CreateMetricLabel(mem_tracker);
}


} // namespace

class MemTracker::TrackerMetrics {
 public:
  explicit TrackerMetrics(const MetricEntityPtr& metric_entity)
      : metric_entity_(metric_entity) {
  }

  void Init(const MemTracker& mem_tracker, const std::string& name_suffix) {
    std::string name = CreateMetricName(mem_tracker);
    if (!name_suffix.empty()) {
      name += "_";
      name += name_suffix;
    }
    metric_ = metric_entity_->FindOrCreateGauge(
        std::unique_ptr<GaugePrototype<int64_t>>(new OwningGaugePrototype<int64_t>(
          metric_entity_->prototype().name(), std::move(name),
          CreateMetricLabel(mem_tracker), MetricUnit::kBytes,
          CreateMetricDescription(mem_tracker), yb::MetricLevel::kInfo)),
        mem_tracker.consumption());
  }

  TrackerMetrics(TrackerMetrics&) = delete;
  void operator=(const TrackerMetrics&) = delete;

  ~TrackerMetrics() {
    metric_entity_->Remove(metric_->prototype());
  }

  MetricEntityPtr metric_entity_;
  scoped_refptr<AtomicGauge<int64_t>> metric_;
};

void MemTracker::PrintTCMallocConfigs() {
#if YB_GOOGLE_TCMALLOC
  LOG(INFO) << "TCMalloc per cpu caches active: "
            << tcmalloc::MallocExtension::PerCpuCachesActive();
  LOG(INFO) << "TCMalloc max per cpu cache size: "
            << tcmalloc::MallocExtension::GetMaxPerCpuCacheSize();
  LOG(INFO) << "TCMalloc max total thread cache bytes: "
            << tcmalloc::MallocExtension::GetMaxTotalThreadCacheBytes();
#endif  // YB_GOOGLE_TCMALLOC
}

void MemTracker::ConfigureTCMalloc() {
  ::yb::ConfigureTCMalloc(MemTracker::GetRootTracker()->limit());
  RegisterTCMallocTraceHooks();
}

void MemTracker::InitRootTrackerOnce() {
  GoogleOnceInit(&root_tracker_once, &MemTracker::CreateRootTracker);
}

void MemTracker::CreateRootTracker() {
  int64_t limit = FLAGS_memory_limit_hard_bytes;
  if (limit == 0) {
    // If no limit is provided, we'll use
    // - 85% of the RAM for tservers.
    // - 10% of the RAM for masters.
    int64_t total_ram;
    CHECK_OK(Env::Default()->GetTotalRAMBytes(&total_ram));
    limit = total_ram * FLAGS_default_memory_limit_to_ram_ratio;
  }

  ConsumptionFunctor consumption_functor;

#if YB_TCMALLOC_ENABLED
  consumption_functor = &GetTCMallocActualHeapSizeBytes;

  if (FLAGS_mem_tracker_tcmalloc_gc_release_bytes < 0) {
    // Allocate 1% of memory to the tcmalloc page heap freelist.
    // On a 4GB RAM machine, the master gets 10%, so 400MB, so 1% is 4MB.
    // On a 16GB RAM machine, the tserver gets 85%, so 13.6GB, so 1% is 136MB, so cap at 128MB.
    FLAGS_mem_tracker_tcmalloc_gc_release_bytes =
        std::min(static_cast<size_t>(1.0 * limit / 100), 128_MB);
  }

  LOG(INFO) << "Creating root MemTracker with garbage collection threshold "
            << FLAGS_mem_tracker_tcmalloc_gc_release_bytes << " bytes";
#endif

  root_tracker = std::make_shared<MemTracker>(
      limit, "root", std::move(consumption_functor), nullptr /* parent */, AddToParent::kTrue,
      CreateMetrics::kFalse);
}

shared_ptr<MemTracker> MemTracker::CreateTracker(int64_t byte_limit,
                                                 const string& id,
                                                 ConsumptionFunctor consumption_functor,
                                                 const shared_ptr<MemTracker>& parent,
                                                 AddToParent add_to_parent,
                                                 CreateMetrics create_metrics) {
  shared_ptr<MemTracker> real_parent = parent ? parent : GetRootTracker();
  return real_parent->CreateChild(
      byte_limit, id, std::move(consumption_functor), MayExist::kFalse, add_to_parent,
      create_metrics);
}

shared_ptr<MemTracker> MemTracker::CreateChild(int64_t byte_limit,
                                               const string& id,
                                               ConsumptionFunctor consumption_functor,
                                               MayExist may_exist,
                                               AddToParent add_to_parent,
                                               CreateMetrics create_metrics) {
  std::lock_guard lock(child_trackers_mutex_);
  if (may_exist) {
    auto result = FindChildUnlocked(id);
    if (result) {
      return result;
    }
  }
  auto result = std::make_shared<MemTracker>(
      byte_limit, id, std::move(consumption_functor), shared_from_this(), add_to_parent,
      create_metrics);
  auto p = child_trackers_.emplace(id, result);
  if (!p.second) {
    auto existing = p.first->second.lock();
    if (existing) {
      LOG(DFATAL) << Format("Duplicate memory tracker (id $0) on parent $1", id, ToString());
      return existing;
    }
    p.first->second = result;
  }

  return result;
}

MemTracker::MemTracker(int64_t byte_limit, const string& id,
                       ConsumptionFunctor consumption_functor, std::shared_ptr<MemTracker> parent,
                       AddToParent add_to_parent, CreateMetrics create_metrics)
    : limit_(byte_limit),
      soft_limit_(limit_ == -1 ? -1 : (limit_ * FLAGS_memory_limit_soft_percentage) / 100),
      id_(id),
      consumption_functor_(std::move(consumption_functor)),
      descr_(Substitute("memory consumption for $0", id)),
      parent_(std::move(parent)),
      rand_(GetRandomSeed32()),
      enable_logging_(FLAGS_mem_tracker_logging),
      log_stack_(FLAGS_mem_tracker_log_stack_trace),
      add_to_parent_(add_to_parent) {
  VLOG(1) << "Creating tracker " << ToString();
  UpdateConsumption();

  all_trackers_.push_back(this);
  if (has_limit()) {
    limit_trackers_.push_back(this);
  }
  if (parent_ && add_to_parent) {
    all_trackers_.insert(
        all_trackers_.end(), parent_->all_trackers_.begin(), parent_->all_trackers_.end());
    limit_trackers_.insert(
        limit_trackers_.end(), parent_->limit_trackers_.begin(), parent_->limit_trackers_.end());
  }

  if (create_metrics) {
    for (MemTracker* tracker = this; tracker; tracker = tracker->parent().get()) {
      if (tracker->metric_entity()) {
        metrics_ = std::make_unique<TrackerMetrics>(tracker->metric_entity());
        metrics_->Init(*this, std::string());
        break;
      }
    }
  }
}

MemTracker::~MemTracker() {
  VLOG(1) << "Destroying tracker " << ToString();
  if (parent_) {
    if (add_to_parent_) {
      parent_->Release(consumption());
    }
  }
}

void MemTracker::UnregisterFromParent() {
  DCHECK(parent_);
  parent_->UnregisterChild(id_);
}

void MemTracker::UnregisterChild(const std::string& id) {
  std::lock_guard lock(child_trackers_mutex_);
  VLOG(1) << "Unregistering child tracker " << id << " from " << id_;
  child_trackers_.erase(id);
}

string MemTracker::ToString() const {
  string s;
  const MemTracker* tracker = this;
  while (tracker) {
    if (s != "") {
      s += "->";
    }
    s += tracker->id();
    tracker = tracker->parent_.get();
  }
  return s;
}

MemTrackerPtr MemTracker::FindTracker(const std::string& id,
                                      const MemTrackerPtr& parent) {
  shared_ptr<MemTracker> real_parent = parent ? parent : GetRootTracker();
  return real_parent->FindChild(id);
}

MemTrackerPtr MemTracker::FindChild(const std::string& id) {
  std::lock_guard lock(child_trackers_mutex_);
  return FindChildUnlocked(id);
}

MemTrackerPtr MemTracker::FindChildUnlocked(const std::string& id) {
  auto it = child_trackers_.find(id);
  if (it != child_trackers_.end()) {
    auto result = it->second.lock();
    if (!result) {
      child_trackers_.erase(it);
    }
    return result;
  }
  return MemTrackerPtr();
}

shared_ptr<MemTracker> MemTracker::FindOrCreateTracker(int64_t byte_limit,
                                                       const string& id,
                                                       const shared_ptr<MemTracker>& parent,
                                                       AddToParent add_to_parent,
                                                       CreateMetrics create_metrics) {
  shared_ptr<MemTracker> real_parent = parent ? parent : GetRootTracker();
  return real_parent->CreateChild(
      byte_limit, id, ConsumptionFunctor(), MayExist::kTrue, add_to_parent, create_metrics);
}

std::vector<MemTrackerPtr> MemTracker::ListChildren() {
  std::vector<MemTrackerPtr> result;
  ListDescendantTrackers(&result, OnlyChildren::kTrue);
  return result;
}

void MemTracker::ListDescendantTrackers(
    std::vector<MemTrackerPtr>* out, OnlyChildren only_children) {
  size_t begin = out->size();
  {
    std::lock_guard lock(child_trackers_mutex_);
    for (auto it = child_trackers_.begin(); it != child_trackers_.end();) {
      auto child = it->second.lock();
      if (child) {
        out->push_back(std::move(child));
        ++it;
      } else {
        it = child_trackers_.erase(it);
      }
    }
  }
  if (!only_children) {
    size_t end = out->size();
    for (size_t i = begin; i != end; ++i) {
      (*out)[i]->ListDescendantTrackers(out);
    }
  }
}

std::vector<MemTrackerPtr> MemTracker::ListTrackers() {
  std::vector<MemTrackerPtr> result;
  auto root = GetRootTracker();
  result.push_back(root);
  root->ListDescendantTrackers(&result);
  return result;
}

bool MemTracker::UpdateConsumption(bool force) {
  if (poll_children_consumption_functors_) {
    poll_children_consumption_functors_();
  }

  if (consumption_functor_) {
    auto now = CoarseMonoClock::now();
    if (force || now > next_consumption_update_) {
      next_consumption_update_ = now + std::chrono::microseconds(GetAtomicFlag(
          &FLAGS_mem_tracker_update_consumption_interval_us));
      auto value = consumption_functor_();
      VLOG(1) << "Setting consumption of tracker " << id_ << " to " << value
              << "from consumption functor";
      consumption_.set_value(value);
      if (metrics_) {
        metrics_->metric_->set_value(value);
      }
    }
    return true;
  }

  return false;
}

void MemTracker::Consume(int64_t bytes) {
  if (bytes < 0) {
    Release(-bytes);
    return;
  }

  if (UpdateConsumption()) {
    return;
  }
  if (bytes == 0) {
    return;
  }
  if (PREDICT_FALSE(enable_logging_)) {
    LogUpdate(true, bytes);
  }
  for (auto& tracker : all_trackers_) {
    if (!tracker->UpdateConsumption()) {
      IncrementBy(bytes, &tracker->consumption_, tracker->metrics_);
      DCHECK_GE(tracker->consumption_.current_value(), 0);
    }
  }
}

bool MemTracker::TryConsume(int64_t bytes, MemTracker** blocking_mem_tracker) {
  UpdateConsumption();
  if (bytes <= 0) {
    return true;
  }
  if (PREDICT_FALSE(enable_logging_)) {
    LogUpdate(true, bytes);
  }

  ssize_t i = 0;
  // Walk the tracker tree top-down, to avoid expanding a limit on a child whose parent
  // won't accommodate the change.
  for (i = all_trackers_.size() - 1; i >= 0; --i) {
    MemTracker *tracker = all_trackers_[i];
    if (tracker->limit_ < 0) {
      IncrementBy(bytes, &tracker->consumption_, tracker->metrics_);
    } else {
      if (!TryIncrementBy(bytes, tracker->limit_, &tracker->consumption_, tracker->metrics_)) {
        // One of the trackers failed, attempt to GC memory or expand our limit. If that
        // succeeds, TryUpdate() again. Bail if either fails.
        if (!tracker->GcMemory(tracker->limit_ - bytes) ||
            tracker->ExpandLimit(bytes)) {
          if (!TryIncrementBy(bytes, tracker->limit_, &tracker->consumption_, tracker->metrics_)) {
            break;
          }
        } else {
          break;
        }
      }
    }
  }
  // Everyone succeeded, return.
  if (i == -1) {
    return true;
  }

  // Someone failed, roll back the ones that succeeded.
  // TODO: this doesn't roll it back completely since the max values for
  // the updated trackers aren't decremented. The max values are only used
  // for error reporting so this is probably okay. Rolling those back is
  // pretty hard; we'd need something like 2PC.
  //
  // TODO: This might leave us with an allocated resource that we can't use. Do we need
  // to adjust the consumption of the query tracker to stop the resource from never
  // getting used by a subsequent TryConsume()?
  for (ssize_t j = all_trackers_.size(); --j > i;) {
    IncrementBy(-bytes, &all_trackers_[j]->consumption_, all_trackers_[j]->metrics_);
  }
  if (blocking_mem_tracker) {
    *blocking_mem_tracker = all_trackers_[i];
  }

  return false;
}

void MemTracker::Release(int64_t bytes) {
  if (bytes < 0) {
    Consume(-bytes);
    return;
  }

  if (PREDICT_FALSE(base::subtle::Barrier_AtomicIncrement(&released_memory_since_gc, bytes) >
                    GetAtomicFlag(&FLAGS_mem_tracker_tcmalloc_gc_release_bytes))) {
    GcTcmallocIfNeeded();
    if (UpdateConsumption(true /* force */)) {
      return;
    }
  } else {
    if (UpdateConsumption()) {
      return;
    }
  }

  if (bytes == 0) {
    return;
  }
  if (PREDICT_FALSE(enable_logging_)) {
    LogUpdate(false, bytes);
  }

  for (auto& tracker : all_trackers_) {
    if (!tracker->UpdateConsumption()) {
      IncrementBy(-bytes, &tracker->consumption_, tracker->metrics_);
      // If a UDF calls FunctionContext::TrackAllocation() but allocates less than the
      // reported amount, the subsequent call to FunctionContext::Free() may cause the
      // process mem tracker to go negative until it is synced back to the tcmalloc
      // metric. Don't blow up in this case. (Note that this doesn't affect non-process
      // trackers since we can enforce that the reported memory usage is internally
      // consistent.)
      DCHECK_GE(tracker->consumption_.current_value(), 0) << "Tracker: " << tracker->ToString();
    }
  }
}

bool MemTracker::AnyLimitExceeded() {
  for (const auto& tracker : limit_trackers_) {
    if (tracker->LimitExceeded()) {
      return true;
    }
  }
  return false;
}

bool MemTracker::LimitExceeded() {
  if (PREDICT_FALSE(CheckLimitExceeded())) {
    return GcMemory(limit_);
  }
  return false;
}

SoftLimitExceededResult MemTracker::SoftLimitExceeded(double* score) {
  // Did we exceed the actual limit?
  if (LimitExceeded()) {
    return {ToString(), true, consumption() * 100.0 / limit()};
  }

  // No soft limit defined.
  if (!has_limit() || limit_ == soft_limit_) {
    return SoftLimitExceededResult::NotExceeded();
  }

  // Are we under the soft limit threshold?
  int64_t usage = consumption();
  if (usage < soft_limit_) {
    return SoftLimitExceededResult::NotExceeded();
  }

  // We're over the threshold; were we randomly chosen to be over the soft limit?
  if (*score == 0.0) {
    *score = RandomUniformReal<double>();
  }
  if (usage + (limit_ - soft_limit_) * *score > limit_ && GcMemory(soft_limit_)) {
    return {ToString(), true, usage * 100.0 / limit()};
  }
  return SoftLimitExceededResult::NotExceeded();
}

SoftLimitExceededResult MemTracker::AnySoftLimitExceeded(double* score) {
  for (MemTracker* t : limit_trackers_) {
    auto result = t->SoftLimitExceeded(score);
    if (result.exceeded) {
      return result;
    }
  }
  return SoftLimitExceededResult::NotExceeded();
}

int64_t MemTracker::SpareCapacity() const {
  int64_t result = std::numeric_limits<int64_t>::max();
  for (const auto& tracker : limit_trackers_) {
    int64_t mem_left = tracker->limit() - tracker->consumption();
    result = std::min(result, mem_left);
  }
  return result;
}

bool MemTracker::GcMemory(int64_t max_consumption) {
  if (max_consumption < 0) {
    // Impossible to GC enough memory to reach the goal.
    return true;
  }

  {
    int64_t current_consumption = GetUpdatedConsumption();
    // Check if someone gc'd before us
    if (current_consumption <= max_consumption) {
      return false;
    }

    // Create vector of alive garbage collectors. Also remove stale garbage collectors.
    GarbageCollectorsContainer<std::shared_ptr<GarbageCollector>> collectors;
    {
      std::lock_guard l(gc_mutex_);
      collectors.reserve(gcs_.size());
      boost::remove_erase_if(
          gcs_,
          [&collectors](const std::weak_ptr<GarbageCollector>& gc_weak) {
              auto gc = gc_weak.lock();
              if (!gc) {
                return true;
              }
              collectors.push_back(std::move(gc));
              return false;
          });
    }

    // Try to free up some memory
    for (const auto& gc : collectors) {
      gc->CollectGarbage(current_consumption - max_consumption);
      current_consumption = GetUpdatedConsumption();
      if (current_consumption <= max_consumption) {
        break;
      }
    }
  }

  int64_t current_consumption = GetUpdatedConsumption();
  if (current_consumption > max_consumption) {
    std::vector<MemTrackerPtr> children;
    {
      std::lock_guard lock(child_trackers_mutex_);
      children.reserve(child_trackers_.size());
      std::erase_if(child_trackers_, [&children](const auto& item) {
        auto child = item.second.lock();
        if (!child) {
          return true;
        }
        children.push_back(std::move(child));
        return false;
      });
    }

    for (const auto& child : children) {
      bool did_gc = child->GcMemory(max_consumption - current_consumption + child->consumption());
      if (did_gc) {
        current_consumption = GetUpdatedConsumption();
        if (current_consumption <= max_consumption) {
          return true;
        }
      }
    }
  }

  return consumption() > max_consumption;
}

void MemTracker::GcTcmallocIfNeeded() {
#ifdef YB_TCMALLOC_ENABLED
  released_memory_since_gc = 0;
  TRACE_EVENT0("process", "MemTracker::GcTcmallocIfNeeded");

  // Number of bytes in the 'NORMAL' free list (i.e reserved by tcmalloc but
  // not in use).
  int64_t bytes_overhead = GetTCMallocPageHeapFreeBytes();
  // Bytes allocated by the application.
  int64_t bytes_used = GetTCMallocCurrentAllocatedBytes();

  int64_t max_overhead = bytes_used * FLAGS_tcmalloc_max_free_bytes_percentage / 100.0;
  if (bytes_overhead > max_overhead) {
    int64_t extra = bytes_overhead - max_overhead;
    while (extra > 0) {
      // Release 1MB at a time, so that tcmalloc releases its page heap lock
      // allowing other threads to make progress. This still disrupts the current
      // thread, but is better than disrupting all.
#if YB_GOOGLE_TCMALLOC
      tcmalloc::MallocExtension::ReleaseMemoryToSystem(1024 * 1024);
#else
      MallocExtension::instance()->ReleaseToSystem(1024 * 1024);
#endif  // YB_GOOGLE_TCMALLOC
      extra -= 1024 * 1024;
    }
  }
#endif  // YB_TCMALLOC_ENABLED
}

string MemTracker::LogUsage(const string& prefix, int64_t usage_threshold, int indent) const {
  stringstream ss;
  ss << prefix << std::string(indent, ' ') << id_ << ":";
  if (CheckLimitExceeded()) {
    ss << " memory limit exceeded.";
  }
  if (limit_ > 0) {
    ss << " Limit=" << HumanReadableNumBytes::ToString(limit_);
  }
  ss << " Consumption=" << HumanReadableNumBytes::ToString(consumption());

  stringstream prefix_ss;
  prefix_ss << prefix << "  ";
  string new_prefix = prefix_ss.str();
  std::lock_guard lock(child_trackers_mutex_);
  for (const auto& p : child_trackers_) {
    auto child = p.second.lock();
    if (child && child->consumption() >= usage_threshold) {
      ss << std::endl;
      ss << child->LogUsage(prefix, usage_threshold, indent + 2);
    }
  }
  return ss.str();
}

void MemTracker::LogMemoryLimits() const {
  LOG(INFO) << StringPrintf("MemTracker: hard memory limit is %.6f GB",
                            (static_cast<float>(limit_) / (1024.0 * 1024.0 * 1024.0)));
  LOG(INFO) << StringPrintf("MemTracker: soft memory limit is %.6f GB",
                            (static_cast<float>(soft_limit_) / (1024.0 * 1024.0 * 1024.0)));
}

void MemTracker::LogUpdate(bool is_consume, int64_t bytes) const {
  stringstream ss;
  ss << id_ << " " << (is_consume ? "Consume: " : "Release: ") << bytes
     << " Consumption: " << consumption() << " Limit: " << limit_;
  if (log_stack_) {
    ss << std::endl << GetStackTrace();
  }
  LOG(ERROR) << ss.str();
}

int64_t MemTracker::GetRootTrackerConsumption() {
  InitRootTrackerOnce();
  return root_tracker->consumption();
}

shared_ptr<MemTracker> MemTracker::GetRootTracker() {
  InitRootTrackerOnce();
  return root_tracker;
}

void MemTracker::SetMetricEntity(
    const MetricEntityPtr& metric_entity, const std::string& name_suffix) {
  if (metrics_) {
    LOG_IF(DFATAL, metric_entity->id() != metrics_->metric_entity_->id())
        << "SetMetricEntity (" << metric_entity->id() << ") while "
        << ToString() << " already has a different metric entity "
        << metrics_->metric_entity_->id();
    return;
  }
  metrics_ = std::make_unique<TrackerMetrics>(metric_entity);
  metrics_->Init(*this, name_suffix);
}

void MemTracker::TEST_SetReleasedMemorySinceGC(int64_t value) {
  released_memory_since_gc = value;
}

scoped_refptr<MetricEntity> MemTracker::metric_entity() const {
  return metrics_ ? metrics_->metric_entity_ : nullptr;
}

const MemTrackerData& CollectMemTrackerData(const MemTrackerPtr& tracker, int depth,
                                            std::vector<MemTrackerData>* output) {
  size_t idx = output->size();
  output->push_back({tracker, depth, 0});

  auto children = tracker->ListChildren();
  std::sort(children.begin(), children.end(), [](const auto& lhs, const auto& rhs) {
    return lhs->id() < rhs->id();
  });

  for (const auto& child : children) {
    const auto& child_data = CollectMemTrackerData(child, depth + 1, output);
    (*output)[idx].consumption_excluded_from_ancestors +=
        child_data.consumption_excluded_from_ancestors;
    if (!child_data.tracker->add_to_parent()) {
      (*output)[idx].consumption_excluded_from_ancestors += child_data.tracker->consumption();
    }
  }

  return (*output)[idx];
}

std::string DumpMemTrackers() {
  std::ostringstream out;
  std::vector<MemTrackerData> trackers;
  CollectMemTrackerData(MemTracker::GetRootTracker(), 0, &trackers);
  for (const auto& data : trackers) {
    const auto& tracker = data.tracker;
    const std::string current_consumption_str =
        HumanReadableNumBytes::ToString(tracker->consumption());
    out << std::string(data.depth, ' ') << tracker->id() << ": ";
    if (!data.consumption_excluded_from_ancestors || data.tracker->UpdateConsumption()) {
      out << current_consumption_str;
    } else {
      auto full_consumption_str = HumanReadableNumBytes::ToString(
          tracker->consumption() + data.consumption_excluded_from_ancestors);
      out << current_consumption_str << " (" << full_consumption_str << ")";
    }
    out << std::endl;
  }
  return out.str();
}

std::string DumpMemoryUsage() {
  std::ostringstream out;
  auto tcmalloc_stats = TcMallocStats();
  if (!tcmalloc_stats.empty()) {
    out << "TCMalloc stats: \n" << tcmalloc_stats << "\n";
  }
  out << "Memory usage: \n" << DumpMemTrackers();
  return out.str();
}

bool CheckMemoryPressureWithLogging(
    const MemTrackerPtr& mem_tracker, double score, const char* error_prefix) {
  const auto soft_limit_exceeded_result = mem_tracker->AnySoftLimitExceeded(&score);
  if (!soft_limit_exceeded_result.exceeded) {
    return true;
  }

  const std::string msg = StringPrintf(
      "Soft memory limit exceeded for %s (at %.2f%% of capacity), score: %.2f",
      soft_limit_exceeded_result.tracker_path.c_str(),
      soft_limit_exceeded_result.current_capacity_pct, score);
  if (soft_limit_exceeded_result.current_capacity_pct >=
      FLAGS_memory_limit_warn_threshold_percentage) {
    YB_LOG_EVERY_N_SECS(WARNING, 1) << error_prefix << msg << THROTTLE_MSG;
  } else {
    YB_LOG_EVERY_N_SECS(INFO, 1) << error_prefix << msg << THROTTLE_MSG;
  }

  return false;
}

} // namespace yb
