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

#include "task_group.h"

#include <fmt/format.h>
#include <gen_cpp/PaloInternalService_types.h>

#include <charconv>
#include <map>
#include <mutex>
#include <ostream>
#include <utility>

#include "common/logging.h"
#include "pipeline/task_queue.h"
#include "pipeline/task_scheduler.h"
#include "runtime/exec_env.h"
#include "runtime/memory/mem_tracker_limiter.h"
#include "util/mem_info.h"
#include "util/parse_util.h"
#include "util/runtime_profile.h"
#include "util/threadpool.h"
#include "vec/exec/scan/scanner_scheduler.h"

namespace doris {
namespace taskgroup {

const static uint64_t CPU_SHARE_DEFAULT_VALUE = 1024;
const static std::string MEMORY_LIMIT_DEFAULT_VALUE = "0%";
const static bool ENABLE_MEMORY_OVERCOMMIT_DEFAULT_VALUE = true;
const static int CPU_HARD_LIMIT_DEFAULT_VALUE = -1;
const static uint64_t CPU_SOFT_LIMIT_DEFAULT_VALUE = 1024;

TaskGroup::TaskGroup(const TaskGroupInfo& tg_info)
        : _id(tg_info.id),
          _name(tg_info.name),
          _version(tg_info.version),
          _memory_limit(tg_info.memory_limit),
          _enable_memory_overcommit(tg_info.enable_memory_overcommit),
          _cpu_share(tg_info.cpu_share),
          _mem_tracker_limiter_pool(MEM_TRACKER_GROUP_NUM),
          _cpu_hard_limit(tg_info.cpu_hard_limit),
          _scan_thread_num(tg_info.scan_thread_num),
          _max_remote_scan_thread_num(tg_info.max_remote_scan_thread_num),
          _min_remote_scan_thread_num(tg_info.min_remote_scan_thread_num) {}

std::string TaskGroup::debug_string() const {
    std::shared_lock<std::shared_mutex> rl {_mutex};
    return fmt::format(
            "TG[id = {}, name = {}, cpu_share = {}, memory_limit = {}, enable_memory_overcommit = "
            "{}, version = {}, cpu_hard_limit = {}, scan_thread_num = "
            "{}, max_remote_scan_thread_num = {}, min_remote_scan_thread_num = {}]",
            _id, _name, cpu_share(), PrettyPrinter::print(_memory_limit, TUnit::BYTES),
            _enable_memory_overcommit ? "true" : "false", _version, cpu_hard_limit(),
            _scan_thread_num, _max_remote_scan_thread_num, _min_remote_scan_thread_num);
}

void TaskGroup::check_and_update(const TaskGroupInfo& tg_info) {
    if (UNLIKELY(tg_info.id != _id)) {
        return;
    }
    {
        std::shared_lock<std::shared_mutex> rl {_mutex};
        if (LIKELY(tg_info.version <= _version)) {
            return;
        }
    }
    {
        std::lock_guard<std::shared_mutex> wl {_mutex};
        if (tg_info.version > _version) {
            _name = tg_info.name;
            _version = tg_info.version;
            _memory_limit = tg_info.memory_limit;
            _enable_memory_overcommit = tg_info.enable_memory_overcommit;
            _cpu_share = tg_info.cpu_share;
            _cpu_hard_limit = tg_info.cpu_hard_limit;
            _scan_thread_num = tg_info.scan_thread_num;
            _max_remote_scan_thread_num = tg_info.max_remote_scan_thread_num;
            _min_remote_scan_thread_num = tg_info.min_remote_scan_thread_num;
        } else {
            return;
        }
    }
}

int64_t TaskGroup::memory_used() {
    int64_t used_memory = 0;
    for (auto& mem_tracker_group : _mem_tracker_limiter_pool) {
        std::lock_guard<std::mutex> l(mem_tracker_group.group_lock);
        for (const auto& tracker : mem_tracker_group.trackers) {
            used_memory += tracker->is_query_cancelled() ? 0 : tracker->consumption();
        }
    }
    return used_memory;
}

void TaskGroup::add_mem_tracker_limiter(std::shared_ptr<MemTrackerLimiter> mem_tracker_ptr) {
    auto group_num = mem_tracker_ptr->group_num();
    std::lock_guard<std::mutex> l(_mem_tracker_limiter_pool[group_num].group_lock);
    _mem_tracker_limiter_pool[group_num].trackers.insert(mem_tracker_ptr);
}

void TaskGroup::remove_mem_tracker_limiter(std::shared_ptr<MemTrackerLimiter> mem_tracker_ptr) {
    auto group_num = mem_tracker_ptr->group_num();
    std::lock_guard<std::mutex> l(_mem_tracker_limiter_pool[group_num].group_lock);
    _mem_tracker_limiter_pool[group_num].trackers.erase(mem_tracker_ptr);
}

int64_t TaskGroup::gc_memory(int64_t need_free_mem, RuntimeProfile* profile) {
    if (need_free_mem <= 0) {
        return 0;
    }
    int64_t used_memory = memory_used();
    int64_t freed_mem = 0;

    std::string cancel_str = fmt::format(
            "work load group memory exceeded limit, group id:{}, name:{}, used:{}, limit:{}, "
            "backend:{}.",
            _id, _name, MemTracker::print_bytes(used_memory),
            MemTracker::print_bytes(_memory_limit), BackendOptions::get_localhost());
    auto cancel_top_overcommit_str = [cancel_str](int64_t mem_consumption,
                                                  const std::string& label) {
        return fmt::format(
                "{} cancel top memory overcommit tracker <{}> consumption {}. execute again after "
                "enough memory, details see be.INFO.",
                cancel_str, label, MemTracker::print_bytes(mem_consumption));
    };
    auto cancel_top_usage_str = [cancel_str](int64_t mem_consumption, const std::string& label) {
        return fmt::format(
                "{} cancel top memory used tracker <{}> consumption {}. execute again after "
                "enough memory, details see be.INFO.",
                cancel_str, label, MemTracker::print_bytes(mem_consumption));
    };

    LOG(INFO) << fmt::format(
            "[MemoryGC] work load group start gc, id:{} name:{}, memory limit: {}, used: {}, "
            "need_free_mem: {}.",
            _id, _name, _memory_limit, used_memory, need_free_mem);
    Defer defer {[&]() {
        LOG(INFO) << fmt::format(
                "[MemoryGC] work load group finished gc, id:{} name:{}, memory limit: {}, used: "
                "{}, need_free_mem: {}, freed memory: {}.",
                _id, _name, _memory_limit, used_memory, need_free_mem, freed_mem);
    }};

    // 1. free top overcommit query
    if (config::enable_query_memory_overcommit) {
        RuntimeProfile* tmq_profile = profile->create_child(
                fmt::format("FreeGroupTopOvercommitQuery:Name {}", _name), true, true);
        freed_mem += MemTrackerLimiter::tg_free_top_overcommit_query(
                need_free_mem - freed_mem, MemTrackerLimiter::Type::QUERY,
                _mem_tracker_limiter_pool, cancel_top_overcommit_str, tmq_profile,
                MemTrackerLimiter::GCType::WORK_LOAD_GROUP);
    }
    if (freed_mem >= need_free_mem) {
        return freed_mem;
    }

    // 2. free top usage query
    RuntimeProfile* tmq_profile =
            profile->create_child(fmt::format("FreeGroupTopUsageQuery:Name {}", _name), true, true);
    freed_mem += MemTrackerLimiter::tg_free_top_memory_query(
            need_free_mem - freed_mem, MemTrackerLimiter::Type::QUERY, _mem_tracker_limiter_pool,
            cancel_top_usage_str, tmq_profile, MemTrackerLimiter::GCType::WORK_LOAD_GROUP);
    if (freed_mem >= need_free_mem) {
        return freed_mem;
    }

    // 3. free top overcommit load
    if (config::enable_query_memory_overcommit) {
        tmq_profile = profile->create_child(
                fmt::format("FreeGroupTopOvercommitLoad:Name {}", _name), true, true);
        freed_mem += MemTrackerLimiter::tg_free_top_overcommit_query(
                need_free_mem - freed_mem, MemTrackerLimiter::Type::LOAD, _mem_tracker_limiter_pool,
                cancel_top_overcommit_str, tmq_profile, MemTrackerLimiter::GCType::WORK_LOAD_GROUP);
        if (freed_mem >= need_free_mem) {
            return freed_mem;
        }
    }

    // 4. free top usage load
    tmq_profile =
            profile->create_child(fmt::format("FreeGroupTopUsageLoad:Name {}", _name), true, true);
    freed_mem += MemTrackerLimiter::tg_free_top_memory_query(
            need_free_mem - freed_mem, MemTrackerLimiter::Type::LOAD, _mem_tracker_limiter_pool,
            cancel_top_usage_str, tmq_profile, MemTrackerLimiter::GCType::WORK_LOAD_GROUP);
    return freed_mem;
}

Status TaskGroupInfo::parse_topic_info(const TWorkloadGroupInfo& workload_group_info,
                                       taskgroup::TaskGroupInfo* task_group_info) {
    // 1 id
    int tg_id = 0;
    if (workload_group_info.__isset.id) {
        tg_id = workload_group_info.id;
    } else {
        return Status::InternalError<false>("workload group id is required");
    }
    task_group_info->id = tg_id;

    // 2 name
    std::string name = "INVALID_NAME";
    if (workload_group_info.__isset.name) {
        name = workload_group_info.name;
    }
    task_group_info->name = name;

    // 3 version
    int version = 0;
    if (workload_group_info.__isset.version) {
        version = workload_group_info.version;
    } else {
        return Status::InternalError<false>("workload group version is required");
    }
    task_group_info->version = version;

    // 4 cpu_share
    uint64_t cpu_share = CPU_SHARE_DEFAULT_VALUE;
    if (workload_group_info.__isset.cpu_share) {
        cpu_share = workload_group_info.cpu_share;
    }
    task_group_info->cpu_share = cpu_share;

    // 5 cpu hard limit
    int cpu_hard_limit = CPU_HARD_LIMIT_DEFAULT_VALUE;
    if (workload_group_info.__isset.cpu_hard_limit) {
        cpu_hard_limit = workload_group_info.cpu_hard_limit;
    }
    task_group_info->cpu_hard_limit = cpu_hard_limit;

    // 6 mem_limit
    std::string mem_limit_str = MEMORY_LIMIT_DEFAULT_VALUE;
    if (workload_group_info.__isset.mem_limit) {
        mem_limit_str = workload_group_info.mem_limit;
    }
    bool is_percent = true;
    int64_t mem_limit =
            ParseUtil::parse_mem_spec(mem_limit_str, -1, MemInfo::mem_limit(), &is_percent);
    task_group_info->memory_limit = mem_limit;

    // 7 mem overcommit
    bool enable_memory_overcommit = ENABLE_MEMORY_OVERCOMMIT_DEFAULT_VALUE;
    if (workload_group_info.__isset.enable_memory_overcommit) {
        enable_memory_overcommit = workload_group_info.enable_memory_overcommit;
    }
    task_group_info->enable_memory_overcommit = enable_memory_overcommit;

    // 8 cpu soft limit or hard limit
    bool enable_cpu_hard_limit = false;
    if (workload_group_info.__isset.enable_cpu_hard_limit) {
        enable_cpu_hard_limit = workload_group_info.enable_cpu_hard_limit;
    }
    task_group_info->enable_cpu_hard_limit = enable_cpu_hard_limit;

    // 9 scan thread num
    task_group_info->scan_thread_num = config::doris_scanner_thread_pool_thread_num;
    if (workload_group_info.__isset.scan_thread_num && workload_group_info.scan_thread_num > 0) {
        task_group_info->scan_thread_num = workload_group_info.scan_thread_num;
    }

    // 10 max remote scan thread num
    task_group_info->max_remote_scan_thread_num = config::doris_scanner_thread_pool_thread_num;
    if (workload_group_info.__isset.max_remote_scan_thread_num &&
        workload_group_info.max_remote_scan_thread_num > 0) {
        task_group_info->max_remote_scan_thread_num =
                workload_group_info.max_remote_scan_thread_num;
    }

    // 11 min remote scan thread num
    task_group_info->min_remote_scan_thread_num = config::doris_scanner_thread_pool_thread_num;
    if (workload_group_info.__isset.min_remote_scan_thread_num &&
        workload_group_info.min_remote_scan_thread_num > 0) {
        task_group_info->min_remote_scan_thread_num =
                workload_group_info.min_remote_scan_thread_num;
    }

    return Status::OK();
}

void TaskGroup::upsert_task_scheduler(taskgroup::TaskGroupInfo* tg_info, ExecEnv* exec_env) {
    uint64_t tg_id = tg_info->id;
    std::string tg_name = tg_info->name;
    int cpu_hard_limit = tg_info->cpu_hard_limit;
    uint64_t cpu_shares = tg_info->cpu_share;
    bool enable_cpu_hard_limit = tg_info->enable_cpu_hard_limit;
    int scan_thread_num = tg_info->scan_thread_num;
    int max_remote_scan_thread_num = tg_info->max_remote_scan_thread_num;
    int min_remote_scan_thread_num = tg_info->min_remote_scan_thread_num;

    std::lock_guard<std::shared_mutex> wlock(_task_sched_lock);
    if (config::doris_cgroup_cpu_path != "" && _cgroup_cpu_ctl == nullptr) {
        std::unique_ptr<CgroupCpuCtl> cgroup_cpu_ctl = std::make_unique<CgroupV1CpuCtl>(tg_id);
        Status ret = cgroup_cpu_ctl->init();
        if (ret.ok()) {
            _cgroup_cpu_ctl = std::move(cgroup_cpu_ctl);
            LOG(INFO) << "[upsert wg thread pool] cgroup init success";
        } else {
            LOG(INFO) << "[upsert wg thread pool] cgroup init failed, gid= " << tg_id
                      << ", reason=" << ret.to_string();
        }
    }

    CgroupCpuCtl* cg_cpu_ctl_ptr = _cgroup_cpu_ctl.get();

    if (_task_sched == nullptr) {
        int32_t executors_size = config::pipeline_executor_size;
        if (executors_size <= 0) {
            executors_size = CpuInfo::num_cores();
        }
        auto task_queue = std::make_shared<pipeline::MultiCoreTaskQueue>(executors_size);
        std::unique_ptr<pipeline::TaskScheduler> pipeline_task_scheduler =
                std::make_unique<pipeline::TaskScheduler>(
                        exec_env, exec_env->get_global_block_scheduler(), std::move(task_queue),
                        "Pipe_" + tg_name, cg_cpu_ctl_ptr);
        Status ret = pipeline_task_scheduler->start();
        if (ret.ok()) {
            _task_sched = std::move(pipeline_task_scheduler);
        } else {
            LOG(INFO) << "[upsert wg thread pool] task scheduler start failed, gid= " << tg_id;
        }
    }

    if (_scan_task_sched == nullptr) {
        std::unique_ptr<vectorized::SimplifiedScanScheduler> scan_scheduler =
                std::make_unique<vectorized::SimplifiedScanScheduler>("Scan_" + tg_name,
                                                                      cg_cpu_ctl_ptr);
        Status ret = scan_scheduler->start();
        if (ret.ok()) {
            _scan_task_sched = std::move(scan_scheduler);
        } else {
            LOG(INFO) << "[upsert wg thread pool] scan scheduler start failed, gid=" << tg_id;
        }
    }
    if (scan_thread_num > 0 && _scan_task_sched) {
        _scan_task_sched->reset_thread_num(scan_thread_num);
    }

    if (_remote_scan_task_sched == nullptr) {
        std::unique_ptr<vectorized::SimplifiedScanScheduler> remote_scan_scheduler =
                std::make_unique<vectorized::SimplifiedScanScheduler>("RScan_" + tg_name,
                                                                      cg_cpu_ctl_ptr);
        Status ret = remote_scan_scheduler->start();
        if (ret.ok()) {
            _remote_scan_task_sched = std::move(remote_scan_scheduler);
        } else {
            LOG(INFO) << "[upsert wg thread pool] remote scan scheduler start failed, gid="
                      << tg_id;
        }
    }
    if (max_remote_scan_thread_num > 0 && _remote_scan_task_sched) {
        _remote_scan_task_sched->reset_max_thread_num(max_remote_scan_thread_num);
    }
    if (min_remote_scan_thread_num > 0 && _remote_scan_task_sched) {
        _remote_scan_task_sched->reset_min_thread_num(min_remote_scan_thread_num);
    }

    if (_non_pipe_thread_pool == nullptr) {
        std::unique_ptr<ThreadPool> thread_pool = nullptr;
        auto ret = ThreadPoolBuilder("nonPip_" + tg_name)
                           .set_min_threads(1)
                           .set_max_threads(config::fragment_pool_thread_num_max)
                           .set_max_queue_size(config::fragment_pool_queue_size)
                           .set_cgroup_cpu_ctl(cg_cpu_ctl_ptr)
                           .build(&thread_pool);
        if (!ret.ok()) {
            LOG(INFO) << "[upsert wg thread pool] create non-pipline thread pool failed, gid="
                      << tg_id;
        } else {
            _non_pipe_thread_pool = std::move(thread_pool);
        }
    }

    // step 6: update cgroup cpu if needed
    if (_cgroup_cpu_ctl) {
        if (enable_cpu_hard_limit) {
            if (cpu_hard_limit > 0) {
                _cgroup_cpu_ctl->update_cpu_hard_limit(cpu_hard_limit);
                _cgroup_cpu_ctl->update_cpu_soft_limit(CPU_SOFT_LIMIT_DEFAULT_VALUE);
            } else {
                LOG(INFO) << "[upsert wg thread pool] enable cpu hard limit but value is illegal: "
                          << cpu_hard_limit << ", gid=" << tg_id;
            }
        } else {
            if (config::enable_cgroup_cpu_soft_limit) {
                _cgroup_cpu_ctl->update_cpu_soft_limit(cpu_shares);
                _cgroup_cpu_ctl->update_cpu_hard_limit(
                        CPU_HARD_LIMIT_DEFAULT_VALUE); // disable cpu hard limit
            }
        }
        _cgroup_cpu_ctl->get_cgroup_cpu_info(&(tg_info->cgroup_cpu_shares),
                                             &(tg_info->cgroup_cpu_hard_limit));
    }
}

void TaskGroup::get_query_scheduler(doris::pipeline::TaskScheduler** exec_sched,
                                    vectorized::SimplifiedScanScheduler** scan_sched,
                                    ThreadPool** non_pipe_thread_pool,
                                    vectorized::SimplifiedScanScheduler** remote_scan_sched) {
    std::shared_lock<std::shared_mutex> rlock(_task_sched_lock);
    *exec_sched = _task_sched.get();
    *scan_sched = _scan_task_sched.get();
    *remote_scan_sched = _remote_scan_task_sched.get();
    *non_pipe_thread_pool = _non_pipe_thread_pool.get();
}

void TaskGroup::try_stop_schedulers() {
    std::shared_lock<std::shared_mutex> rlock(_task_sched_lock);
    if (_task_sched) {
        _task_sched->stop();
    }
    if (_scan_task_sched) {
        _scan_task_sched->stop();
    }
    if (_remote_scan_task_sched) {
        _remote_scan_task_sched->stop();
    }
    if (_non_pipe_thread_pool) {
        _non_pipe_thread_pool->shutdown();
        _non_pipe_thread_pool->wait();
    }
}

} // namespace taskgroup
} // namespace doris
