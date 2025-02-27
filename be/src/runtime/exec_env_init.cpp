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

#include "agent/cgroups_mgr.h"
#include "common/config.h"
#include "common/logging.h"
#include "gen_cpp/BackendService.h"
#include "gen_cpp/HeartbeatService_types.h"
#include "gen_cpp/TPaloBrokerService.h"
#include "olap/page_cache.h"
#include "olap/segment_loader.h"
#include "olap/storage_engine.h"
#include "olap/storage_policy_mgr.h"
#include "pipeline/task_scheduler.h"
#include "runtime/block_spill_manager.h"
#include "runtime/broker_mgr.h"
#include "runtime/cache/result_cache.h"
#include "runtime/client_cache.h"
#include "runtime/exec_env.h"
#include "runtime/external_scan_context_mgr.h"
#include "runtime/fold_constant_executor.h"
#include "runtime/fragment_mgr.h"
#include "runtime/heartbeat_flags.h"
#include "runtime/load_channel_mgr.h"
#include "runtime/load_path_mgr.h"
#include "runtime/memory/mem_tracker.h"
#include "runtime/result_buffer_mgr.h"
#include "runtime/result_queue_mgr.h"
#include "runtime/routine_load/routine_load_task_executor.h"
#include "runtime/small_file_mgr.h"
#include "runtime/stream_load/load_stream_mgr.h"
#include "runtime/stream_load/new_load_stream_mgr.h"
#include "runtime/stream_load/stream_load_executor.h"
#include "runtime/thread_resource_mgr.h"
#include "runtime/tmp_file_mgr.h"
#include "util/bfd_parser.h"
#include "util/brpc_client_cache.h"
#include "util/doris_metrics.h"
#include "util/mem_info.h"
#include "util/metrics.h"
#include "util/parse_util.h"
#include "util/pretty_printer.h"
#include "util/priority_thread_pool.hpp"
#include "util/priority_work_stealing_thread_pool.hpp"
#include "vec/exec/scan/scanner_scheduler.h"
#include "vec/runtime/vdata_stream_mgr.h"

#if !defined(__SANITIZE_ADDRESS__) && !defined(ADDRESS_SANITIZER) && !defined(LEAK_SANITIZER) && \
        !defined(THREAD_SANITIZER) && !defined(USE_JEMALLOC)
#include "runtime/memory/tcmalloc_hook.h"
#endif

namespace doris {

DEFINE_GAUGE_METRIC_PROTOTYPE_2ARG(scanner_thread_pool_queue_size, MetricUnit::NOUNIT);
DEFINE_GAUGE_METRIC_PROTOTYPE_2ARG(send_batch_thread_pool_thread_num, MetricUnit::NOUNIT);
DEFINE_GAUGE_METRIC_PROTOTYPE_2ARG(send_batch_thread_pool_queue_size, MetricUnit::NOUNIT);
DEFINE_GAUGE_METRIC_PROTOTYPE_2ARG(download_cache_thread_pool_thread_num, MetricUnit::NOUNIT);
DEFINE_GAUGE_METRIC_PROTOTYPE_2ARG(download_cache_thread_pool_queue_size, MetricUnit::NOUNIT);

Status ExecEnv::init(ExecEnv* env, const std::vector<StorePath>& store_paths) {
    return env->_init(store_paths);
}

Status ExecEnv::_init(const std::vector<StorePath>& store_paths) {
    //Only init once before be destroyed
    if (_is_init) {
        return Status::OK();
    }
    _store_paths = store_paths;
    // path_name => path_index
    for (int i = 0; i < store_paths.size(); i++) {
        _store_path_map[store_paths[i].path] = i;
    }

    _external_scan_context_mgr = new ExternalScanContextMgr(this);
    _vstream_mgr = new doris::vectorized::VDataStreamMgr();
    _result_mgr = new ResultBufferMgr();
    _result_queue_mgr = new ResultQueueMgr();
    _backend_client_cache = new BackendServiceClientCache(config::max_client_cache_size_per_host);
    _frontend_client_cache = new FrontendServiceClientCache(config::max_client_cache_size_per_host);
    _broker_client_cache = new BrokerServiceClientCache(config::max_client_cache_size_per_host);
    _thread_mgr = new ThreadResourceMgr();

    ThreadPoolBuilder("SendBatchThreadPool")
            .set_min_threads(config::send_batch_thread_pool_thread_num)
            .set_max_threads(config::send_batch_thread_pool_thread_num)
            .set_max_queue_size(config::send_batch_thread_pool_queue_size)
            .build(&_send_batch_thread_pool);

    init_download_cache_required_components();

    RETURN_IF_ERROR(init_pipeline_task_scheduler());
    _scanner_scheduler = new doris::vectorized::ScannerScheduler();

    _cgroups_mgr = new CgroupsMgr(this, config::doris_cgroups);
    _fragment_mgr = new FragmentMgr(this);
    _result_cache = new ResultCache(config::query_cache_max_size_mb,
                                    config::query_cache_elasticity_size_mb);
    _master_info = new TMasterInfo();
    _load_path_mgr = new LoadPathMgr(this);
    _tmp_file_mgr = new TmpFileMgr(this);
    _bfd_parser = BfdParser::create();
    _broker_mgr = new BrokerMgr(this);
    _load_channel_mgr = new LoadChannelMgr();
    _load_stream_mgr = new LoadStreamMgr();
    _new_load_stream_mgr = new NewLoadStreamMgr();
    _internal_client_cache = new BrpcClientCache<PBackendService_Stub>();
    _function_client_cache = new BrpcClientCache<PFunctionService_Stub>();
    _stream_load_executor = new StreamLoadExecutor(this);
    _routine_load_task_executor = new RoutineLoadTaskExecutor(this);
    _small_file_mgr = new SmallFileMgr(this, config::small_file_dir);
    _storage_policy_mgr = new StoragePolicyMgr();
    _block_spill_mgr = new BlockSpillManager(_store_paths);

    _backend_client_cache->init_metrics("backend");
    _frontend_client_cache->init_metrics("frontend");
    _broker_client_cache->init_metrics("broker");
    _result_mgr->init();
    _cgroups_mgr->init_cgroups();
    Status status = _load_path_mgr->init();
    if (!status.ok()) {
        LOG(ERROR) << "load path mgr init failed." << status;
        exit(-1);
    }
    _broker_mgr->init();
    _small_file_mgr->init();
    _scanner_scheduler->init(this);

    _init_mem_env();

    RETURN_IF_ERROR(_load_channel_mgr->init(MemInfo::mem_limit()));
    _heartbeat_flags = new HeartbeatFlags();
    _register_metrics();
    _is_init = true;
    return Status::OK();
}

Status ExecEnv::init_pipeline_task_scheduler() {
    auto executors_size = config::pipeline_executor_size;
    if (executors_size <= 0) {
        executors_size = CpuInfo::num_cores();
    }
    auto t_queue = std::make_shared<pipeline::TaskQueue>(executors_size);
    auto b_scheduler = std::make_shared<pipeline::BlockedTaskScheduler>(t_queue);
    _pipeline_task_scheduler = new pipeline::TaskScheduler(this, b_scheduler, t_queue);
    RETURN_IF_ERROR(_pipeline_task_scheduler->start());
    return Status::OK();
}

Status ExecEnv::_init_mem_env() {
    bool is_percent = false;
    std::stringstream ss;
    // 1. init mem tracker
    _orphan_mem_tracker =
            std::make_shared<MemTrackerLimiter>(MemTrackerLimiter::Type::GLOBAL, "Orphan");
    _orphan_mem_tracker_raw = _orphan_mem_tracker.get();
    thread_context()->thread_mem_tracker_mgr->init();
#if defined(USE_MEM_TRACKER) && !defined(__SANITIZE_ADDRESS__) && !defined(ADDRESS_SANITIZER) && \
        !defined(LEAK_SANITIZER) && !defined(THREAD_SANITIZER) && !defined(USE_JEMALLOC)
    if (doris::config::enable_tcmalloc_hook) {
        init_hook();
    }
#endif

    // 2. init buffer pool
    if (!BitUtil::IsPowerOf2(config::min_buffer_size)) {
        ss << "Config min_buffer_size must be a power-of-two: " << config::min_buffer_size;
        return Status::InternalError(ss.str());
    }

    // 3. init storage page cache
    int64_t storage_cache_limit =
            ParseUtil::parse_mem_spec(config::storage_page_cache_limit, MemInfo::mem_limit(),
                                      MemInfo::physical_mem(), &is_percent);
    while (!is_percent && storage_cache_limit > MemInfo::mem_limit() / 2) {
        storage_cache_limit = storage_cache_limit / 2;
    }
    int32_t index_percentage = config::index_page_cache_percentage;
    uint32_t num_shards = config::storage_page_cache_shard_size;
    StoragePageCache::create_global_cache(storage_cache_limit, index_percentage, num_shards);
    LOG(INFO) << "Storage page cache memory limit: "
              << PrettyPrinter::print(storage_cache_limit, TUnit::BYTES)
              << ", origin config value: " << config::storage_page_cache_limit;

    uint64_t fd_number = config::min_file_descriptor_number;
    struct rlimit l;
    int ret = getrlimit(RLIMIT_NOFILE, &l);
    if (ret != 0) {
        LOG(WARNING) << "call getrlimit() failed. errno=" << strerror(errno)
                     << ", use default configuration instead.";
    } else {
        fd_number = static_cast<uint64_t>(l.rlim_cur);
    }
    // SegmentLoader caches segments in rowset granularity. So the size of
    // opened files will greater than segment_cache_capacity.
    uint64_t segment_cache_capacity = fd_number / 3 * 2;
    LOG(INFO) << "segment_cache_capacity = fd_number / 3 * 2, fd_number: " << fd_number
              << " segment_cache_capacity: " << segment_cache_capacity;
    SegmentLoader::create_global_instance(segment_cache_capacity);

    // 4. init other managers
    RETURN_IF_ERROR(_tmp_file_mgr->init());
    RETURN_IF_ERROR(_block_spill_mgr->init());

    // 5. init chunk allocator
    if (!BitUtil::IsPowerOf2(config::min_chunk_reserved_bytes)) {
        ss << "Config min_chunk_reserved_bytes must be a power-of-two: "
           << config::min_chunk_reserved_bytes;
        return Status::InternalError(ss.str());
    }

    int64_t chunk_reserved_bytes_limit =
            ParseUtil::parse_mem_spec(config::chunk_reserved_bytes_limit, MemInfo::mem_limit(),
                                      MemInfo::physical_mem(), &is_percent);
    chunk_reserved_bytes_limit =
            BitUtil::RoundDown(chunk_reserved_bytes_limit, config::min_chunk_reserved_bytes);
    ChunkAllocator::init_instance(chunk_reserved_bytes_limit);
    LOG(INFO) << "Chunk allocator memory limit: "
              << PrettyPrinter::print(chunk_reserved_bytes_limit, TUnit::BYTES)
              << ", origin config value: " << config::chunk_reserved_bytes_limit;
    return Status::OK();
}

void ExecEnv::init_download_cache_buf() {
    std::unique_ptr<char[]> download_cache_buf(new char[config::download_cache_buffer_size]);
    memset(download_cache_buf.get(), 0, config::download_cache_buffer_size);
    _download_cache_buf_map[_serial_download_cache_thread_token.get()] =
            std::move(download_cache_buf);
}

void ExecEnv::init_download_cache_required_components() {
    ThreadPoolBuilder("DownloadCacheThreadPool")
            .set_min_threads(1)
            .set_max_threads(config::download_cache_thread_pool_thread_num)
            .set_max_queue_size(config::download_cache_thread_pool_queue_size)
            .build(&_download_cache_thread_pool);
    set_serial_download_cache_thread_token();
    init_download_cache_buf();
}

void ExecEnv::_register_metrics() {
    REGISTER_HOOK_METRIC(send_batch_thread_pool_thread_num,
                         [this]() { return _send_batch_thread_pool->num_threads(); });

    REGISTER_HOOK_METRIC(send_batch_thread_pool_queue_size,
                         [this]() { return _send_batch_thread_pool->get_queue_size(); });

    REGISTER_HOOK_METRIC(download_cache_thread_pool_thread_num,
                         [this]() { return _download_cache_thread_pool->num_threads(); });

    REGISTER_HOOK_METRIC(download_cache_thread_pool_queue_size,
                         [this]() { return _download_cache_thread_pool->get_queue_size(); });
}

void ExecEnv::_deregister_metrics() {
    DEREGISTER_HOOK_METRIC(scanner_thread_pool_queue_size);
    DEREGISTER_HOOK_METRIC(send_batch_thread_pool_thread_num);
    DEREGISTER_HOOK_METRIC(send_batch_thread_pool_queue_size);
    DEREGISTER_HOOK_METRIC(download_cache_thread_pool_thread_num);
    DEREGISTER_HOOK_METRIC(download_cache_thread_pool_queue_size);
}

void ExecEnv::_destroy() {
    //Only destroy once after init
    if (!_is_init) {
        return;
    }
    _deregister_metrics();
    SAFE_DELETE(_internal_client_cache);
    SAFE_DELETE(_function_client_cache);
    SAFE_DELETE(_load_stream_mgr);
    SAFE_DELETE(_load_channel_mgr);
    SAFE_DELETE(_broker_mgr);
    SAFE_DELETE(_bfd_parser);
    SAFE_DELETE(_tmp_file_mgr);
    SAFE_DELETE(_load_path_mgr);
    SAFE_DELETE(_master_info);
    SAFE_DELETE(_fragment_mgr);
    SAFE_DELETE(_pipeline_task_scheduler);
    SAFE_DELETE(_cgroups_mgr);
    SAFE_DELETE(_thread_mgr);
    SAFE_DELETE(_broker_client_cache);
    SAFE_DELETE(_frontend_client_cache);
    SAFE_DELETE(_backend_client_cache);
    SAFE_DELETE(_result_mgr);
    SAFE_DELETE(_result_queue_mgr);
    SAFE_DELETE(_stream_load_executor);
    SAFE_DELETE(_routine_load_task_executor);
    SAFE_DELETE(_external_scan_context_mgr);
    SAFE_DELETE(_heartbeat_flags);
    SAFE_DELETE(_scanner_scheduler);

    _is_init = false;
}

void ExecEnv::destroy(ExecEnv* env) {
    env->_destroy();
}

} // namespace doris
