// Copyright 2026 Alibaba Cloud and its affiliates
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <latch>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include "gflags/gflags.h"
#include "glog/logging.h"

#include "master_client.h"

static constexpr uint64_t KiB = 1024;
static constexpr uint64_t MiB = 1024 * KiB;
static constexpr uint64_t GiB = 1024 * MiB;
static constexpr uintptr_t kSegmentBase = 0x200000000ULL;
static constexpr size_t kNumMetadataShards = 1024;

DEFINE_string(master_server, "127.0.0.1:50051", "Master server address");
DEFINE_string(label, "run", "Label written to summary output");
DEFINE_string(csv_path, "", "Optional CSV output path");
DEFINE_string(operation, "BatchPutEnd",
              "Completion RPC to measure: BatchPutEnd or BatchPutRevoke");
DEFINE_string(routing_mode, "ungrouped",
              "Metadata routing mode: ungrouped, same_shard_keys, or "
              "grouped_single_shard");
DEFINE_string(key_prefix, "batch_put_completion_bench",
              "Prefix for generated object keys");
DEFINE_uint64(num_segments, 16, "Number of mounted segments");
DEFINE_uint64(segment_size, 8 * GiB, "Size of each mounted segment");
DEFINE_uint64(workers, 16, "Number of concurrent benchmark workers");
DEFINE_uint64(batch_size, 512, "Keys per batch completion request");
DEFINE_uint64(value_size, 4096,
              "Object value size used for BatchPutStart allocation");
DEFINE_uint64(warmup_requests, 8, "Warmup requests per worker");
DEFINE_uint64(requests_per_worker, 48, "Measured requests per worker");
DEFINE_uint64(max_prepare_retries, 16,
              "Max retries when a full BatchPutStart batch cannot be staged");

namespace {

enum class CompletionOperation { kBatchPutEnd, kBatchPutRevoke };
enum class RoutingMode {
    kUngrouped,
    kSameShardKeys,
    kGroupedSingleShard,
};

std::string gRunNamespace;

static inline void unset_cpu_affinity() {
    cpu_set_t cpuset;
    memset(&cpuset, -1, sizeof(cpuset));
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

CompletionOperation ParseOperation() {
    if (FLAGS_operation == "BatchPutEnd") {
        return CompletionOperation::kBatchPutEnd;
    }
    if (FLAGS_operation == "BatchPutRevoke") {
        return CompletionOperation::kBatchPutRevoke;
    }
    throw std::invalid_argument(
        "operation must be BatchPutEnd or "
        "BatchPutRevoke");
}

RoutingMode ParseRoutingMode() {
    if (FLAGS_routing_mode == "ungrouped") {
        return RoutingMode::kUngrouped;
    }
    if (FLAGS_routing_mode == "same_shard_keys") {
        return RoutingMode::kSameShardKeys;
    }
    if (FLAGS_routing_mode == "grouped_single_shard") {
        return RoutingMode::kGroupedSingleShard;
    }
    throw std::invalid_argument(
        "routing_mode must be ungrouped, same_shard_keys, or "
        "grouped_single_shard");
}

class SegmentClient {
   public:
    SegmentClient(const std::string& name, const std::string& master_server,
                  uintptr_t segment_base, uint64_t segment_size)
        : master_client_(mooncake::generate_uuid()) {
        auto ec = master_client_.Connect(master_server);
        if (ec != mooncake::ErrorCode::OK) {
            throw std::runtime_error("Cannot connect to master server at " +
                                     master_server + ", ec=" + toString(ec));
        }

        segment_.id = mooncake::generate_uuid();
        segment_.name = name;
        segment_.base = segment_base;
        segment_.size = segment_size;
        segment_.te_endpoint = name;
        auto mount_ec = master_client_.MountSegment(segment_);
        if (!mount_ec.has_value()) {
            throw std::runtime_error("Failed to mount segment " + name +
                                     ", ec=" + toString(mount_ec.error()));
        }
    }

    ~SegmentClient() {
        if (remount_future_.valid()) {
            remount_future_.wait();
        }

        auto unmount_result = master_client_.UnmountSegment(segment_.id);
        if (!unmount_result.has_value()) {
            LOG(ERROR) << "Failed to unmount segment " << segment_.name;
        }
    }

    void Ping() {
        if (remount_future_.valid() &&
            remount_future_.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            remount_future_.get();
            remount_future_ = std::future<void>();
        }

        auto ping_result = master_client_.Ping();
        if (!ping_result.has_value()) {
            throw std::runtime_error("Failed to ping master server");
        }

        if (ping_result.value().client_status ==
                mooncake::ClientStatus::NEED_REMOUNT &&
            !remount_future_.valid()) {
            remount_future_ = std::async(std::launch::async, [&]() {
                auto remount_ec = master_client_.ReMountSegment({segment_});
                if (!remount_ec.has_value()) {
                    throw std::runtime_error("Failed to remount segment");
                }
            });
        }
    }

   private:
    mooncake::MasterClient master_client_;
    mooncake::Segment segment_;
    std::future<void> remount_future_;
};

struct BatchFixture {
    std::vector<std::string> keys;
};

struct WorkerSummary {
    std::vector<double> latency_us;
    uint64_t prepare_retries = 0;
    uint64_t warmup_batches = 0;
};

struct SummaryRow {
    std::string label;
    std::string operation;
    std::string routing_mode;
    uint64_t workers = 0;
    uint64_t batch_size = 0;
    uint64_t warmup_requests = 0;
    uint64_t requests_per_worker = 0;
    uint64_t total_requests = 0;
    uint64_t total_keys = 0;
    uint64_t prepare_retries = 0;
    double duration_sec = 0.0;
    double batch_per_sec = 0.0;
    double keys_per_sec = 0.0;
    double avg_us = 0.0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
    double max_us = 0.0;
    double avg_ns_per_key = 0.0;
};

void ValidateFlags() {
    if (FLAGS_num_segments == 0 || FLAGS_segment_size == 0 ||
        FLAGS_workers == 0 || FLAGS_batch_size == 0 || FLAGS_value_size == 0 ||
        FLAGS_requests_per_worker == 0 || FLAGS_max_prepare_retries == 0) {
        throw std::invalid_argument(
            "num_segments, segment_size, workers, batch_size, value_size, "
            "requests_per_worker, and max_prepare_retries must be positive");
    }
}

std::vector<std::vector<uint64_t>> MakeSliceLengths() {
    return std::vector<std::vector<uint64_t>>(
        FLAGS_batch_size, std::vector<uint64_t>{FLAGS_value_size});
}

std::vector<std::string> MakeKeys(uint64_t key_base) {
    std::vector<std::string> keys;
    keys.reserve(FLAGS_batch_size);
    for (uint64_t i = 0; i < FLAGS_batch_size; ++i) {
        keys.push_back(gRunNamespace + "_k_" + std::to_string(key_base + i));
    }
    return keys;
}

size_t GetDefaultTenantShard(const std::string& key) {
    return std::hash<std::string>{}(key) % kNumMetadataShards;
}

size_t PickBatchShard(uint64_t worker_idx, uint64_t batch_idx) {
    const uint64_t mixed = (worker_idx + 1) * 0x9e3779b97f4a7c15ULL ^
                           (batch_idx + 1) * 0xbf58476d1ce4e5b9ULL;
    return std::hash<uint64_t>{}(mixed) % kNumMetadataShards;
}

std::vector<std::string> MakeSameShardKeys(uint64_t worker_idx,
                                           uint64_t batch_idx,
                                           std::atomic<uint64_t>& next_key_id) {
    const size_t target_shard = PickBatchShard(worker_idx, batch_idx);
    std::vector<std::string> keys;
    keys.reserve(FLAGS_batch_size);

    while (keys.size() < FLAGS_batch_size) {
        const uint64_t candidate_id = next_key_id.fetch_add(1);
        std::string key = gRunNamespace + "_k_" + std::to_string(candidate_id);
        if (GetDefaultTenantShard(key) == target_shard) {
            keys.push_back(std::move(key));
        }
    }

    return keys;
}

std::string MakeBatchGroupId(uint64_t worker_idx, uint64_t batch_idx) {
    return gRunNamespace + "_g_" + std::to_string(worker_idx) + "_" +
           std::to_string(batch_idx);
}

mooncake::ReplicateConfig MakeConfig(RoutingMode routing_mode,
                                     uint64_t worker_idx, uint64_t batch_idx) {
    mooncake::ReplicateConfig config;
    if (routing_mode == RoutingMode::kGroupedSingleShard) {
        config.group_ids = std::vector<std::string>(
            FLAGS_batch_size, MakeBatchGroupId(worker_idx, batch_idx));
    }
    return config;
}

template <typename ResultT>
void RequireBatchSuccess(
    const std::vector<tl::expected<ResultT, mooncake::ErrorCode>>& results,
    size_t expected_size, const std::string& action) {
    if (results.size() != expected_size) {
        std::ostringstream ss;
        ss << action << " returned " << results.size() << " results, expected "
           << expected_size;
        throw std::runtime_error(ss.str());
    }
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].has_value()) {
            std::ostringstream ss;
            ss << action << " failed at index " << i
               << ", ec=" << toString(results[i].error());
            throw std::runtime_error(ss.str());
        }
    }
}

void CleanupCompletedBatch(mooncake::MasterClient& client,
                           const std::vector<std::string>& keys) {
    auto results = client.BatchRemove(keys, true);
    RequireBatchSuccess(results, keys.size(), "BatchRemove");
}

void RevokePreparedBatch(mooncake::MasterClient& client,
                         const std::vector<std::string>& keys) {
    auto results = client.BatchPutRevoke(keys, mooncake::ReplicaType::MEMORY);
    RequireBatchSuccess(results, keys.size(), "BatchPutRevoke(cleanup)");
}

BatchFixture PrepareBatch(
    mooncake::MasterClient& client,
    const std::vector<std::vector<uint64_t>>& slice_lengths,
    RoutingMode routing_mode, uint64_t worker_idx, uint64_t batch_idx,
    std::atomic<uint64_t>& next_key_id, uint64_t& prepare_retries) {
    for (uint64_t attempt = 0; attempt < FLAGS_max_prepare_retries; ++attempt) {
        std::vector<std::string> keys;
        if (routing_mode == RoutingMode::kSameShardKeys) {
            keys = MakeSameShardKeys(worker_idx, batch_idx, next_key_id);
        } else {
            const uint64_t key_base = next_key_id.fetch_add(FLAGS_batch_size);
            keys = MakeKeys(key_base);
        }
        auto config = MakeConfig(routing_mode, worker_idx, batch_idx);
        auto results = client.BatchPutStart(keys, slice_lengths, config);
        if (results.size() != keys.size()) {
            std::ostringstream ss;
            ss << "BatchPutStart returned " << results.size()
               << " results, expected " << keys.size();
            throw std::runtime_error(ss.str());
        }

        std::vector<std::string> started_keys;
        started_keys.reserve(keys.size());
        bool full_success = true;
        for (size_t i = 0; i < results.size(); ++i) {
            if (results[i].has_value()) {
                started_keys.push_back(keys[i]);
            } else {
                full_success = false;
            }
        }

        if (full_success) {
            return {.keys = std::move(keys)};
        }

        ++prepare_retries;
        if (!started_keys.empty()) {
            RevokePreparedBatch(client, started_keys);
        }
    }

    throw std::runtime_error(
        "Failed to stage a full BatchPutStart batch after max_prepare_retries");
}

void RunCompletion(mooncake::MasterClient& client,
                   const std::vector<std::string>& keys,
                   CompletionOperation operation) {
    if (operation == CompletionOperation::kBatchPutEnd) {
        auto results = client.BatchPutEnd(keys, mooncake::ReplicaType::MEMORY);
        RequireBatchSuccess(results, keys.size(), "BatchPutEnd");
        return;
    }

    auto results = client.BatchPutRevoke(keys, mooncake::ReplicaType::MEMORY);
    RequireBatchSuccess(results, keys.size(), "BatchPutRevoke");
}

double MeasureCompletionLatencyUs(mooncake::MasterClient& client,
                                  const std::vector<std::string>& keys,
                                  CompletionOperation operation) {
    const auto started_at = std::chrono::steady_clock::now();
    RunCompletion(client, keys, operation);
    const auto finished_at = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(finished_at - started_at)
        .count();
}

double Percentile(const std::vector<double>& sorted_values, double percentile) {
    if (sorted_values.empty()) {
        return 0.0;
    }
    if (sorted_values.size() == 1) {
        return sorted_values.front();
    }

    const double rank = percentile * (sorted_values.size() - 1);
    const auto low = static_cast<size_t>(std::floor(rank));
    const auto high = static_cast<size_t>(std::ceil(rank));
    if (low == high) {
        return sorted_values[low];
    }
    return sorted_values[low] +
           (sorted_values[high] - sorted_values[low]) * (rank - low);
}

SummaryRow BuildSummary(const std::vector<WorkerSummary>& worker_summaries,
                        double duration_sec) {
    std::vector<double> latencies_us;
    latencies_us.reserve(FLAGS_workers * FLAGS_requests_per_worker);

    uint64_t prepare_retries = 0;
    for (const auto& worker_summary : worker_summaries) {
        prepare_retries += worker_summary.prepare_retries;
        latencies_us.insert(latencies_us.end(),
                            worker_summary.latency_us.begin(),
                            worker_summary.latency_us.end());
    }

    std::sort(latencies_us.begin(), latencies_us.end());
    const double latency_sum =
        std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    SummaryRow row;
    row.label = FLAGS_label;
    row.operation = FLAGS_operation;
    row.routing_mode = FLAGS_routing_mode;
    row.workers = FLAGS_workers;
    row.batch_size = FLAGS_batch_size;
    row.warmup_requests = FLAGS_warmup_requests;
    row.requests_per_worker = FLAGS_requests_per_worker;
    row.total_requests = static_cast<uint64_t>(latencies_us.size());
    row.total_keys = row.total_requests * FLAGS_batch_size;
    row.prepare_retries = prepare_retries;
    row.duration_sec = duration_sec;
    row.batch_per_sec =
        duration_sec > 0.0
            ? static_cast<double>(row.total_requests) / duration_sec
            : 0.0;
    row.keys_per_sec = duration_sec > 0.0
                           ? static_cast<double>(row.total_keys) / duration_sec
                           : 0.0;
    row.avg_us = latencies_us.empty() ? 0.0 : latency_sum / latencies_us.size();
    row.p50_us = Percentile(latencies_us, 0.50);
    row.p95_us = Percentile(latencies_us, 0.95);
    row.p99_us = Percentile(latencies_us, 0.99);
    row.max_us = latencies_us.empty() ? 0.0 : latencies_us.back();
    row.avg_ns_per_key =
        FLAGS_batch_size > 0 ? (row.avg_us * 1000.0) / FLAGS_batch_size : 0.0;
    return row;
}

void PrintSummary(const SummaryRow& row) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\nBatch put completion latency summary\n";
    std::cout << "label=" << row.label << ", operation=" << row.operation
              << ", routing_mode=" << row.routing_mode
              << ", workers=" << row.workers
              << ", batch_size=" << row.batch_size
              << ", warmup_requests=" << row.warmup_requests
              << ", requests_per_worker=" << row.requests_per_worker << "\n";
    std::cout << "total_requests=" << row.total_requests
              << ", total_keys=" << row.total_keys
              << ", duration_sec=" << row.duration_sec
              << ", prepare_retries=" << row.prepare_retries << "\n";
    std::cout << "\n| avg us/batch | p50 us | p95 us | p99 us | max us | avg "
                 "ns/key | batch/s | keys/s |\n";
    std::cout << "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
    std::cout << "| " << row.avg_us << " | " << row.p50_us << " | "
              << row.p95_us << " | " << row.p99_us << " | " << row.max_us
              << " | " << row.avg_ns_per_key << " | " << row.batch_per_sec
              << " | " << row.keys_per_sec << " |\n";
    std::cout << "\nRESULT avg_us=" << row.avg_us << " p50_us=" << row.p50_us
              << " p95_us=" << row.p95_us << " p99_us=" << row.p99_us
              << " max_us=" << row.max_us
              << " avg_ns_per_key=" << row.avg_ns_per_key
              << " batch_per_sec=" << row.batch_per_sec
              << " keys_per_sec=" << row.keys_per_sec
              << " prepare_retries=" << row.prepare_retries << "\n";
}

void WriteCsv(const SummaryRow& row) {
    if (FLAGS_csv_path.empty()) {
        return;
    }

    const bool need_header = !std::ifstream(FLAGS_csv_path).good() ||
                             std::ifstream(FLAGS_csv_path).peek() ==
                                 std::ifstream::traits_type::eof();
    std::ofstream out(FLAGS_csv_path, std::ios::app);
    if (!out) {
        throw std::runtime_error("Cannot open CSV output path: " +
                                 FLAGS_csv_path);
    }

    if (need_header) {
        out << "label,operation,routing_mode,workers,batch_size,"
               "warmup_requests,requests_per_worker,total_requests,total_keys,"
               "prepare_retries,duration_sec,batch_per_sec,keys_per_sec,"
               "avg_us,p50_us,p95_us,p99_us,max_us,avg_ns_per_key\n";
    }

    out << std::fixed << std::setprecision(6);
    out << row.label << "," << row.operation << "," << row.routing_mode << ","
        << row.workers << "," << row.batch_size << "," << row.warmup_requests
        << "," << row.requests_per_worker << "," << row.total_requests << ","
        << row.total_keys << "," << row.prepare_retries << ","
        << row.duration_sec << "," << row.batch_per_sec << ","
        << row.keys_per_sec << "," << row.avg_us << "," << row.p50_us << ","
        << row.p95_us << "," << row.p99_us << "," << row.max_us << ","
        << row.avg_ns_per_key << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    google::InitGoogleLogging("BatchPutCompletionBench");
    FLAGS_logtostderr = true;
    gflags::ParseCommandLineFlags(&argc, &argv, false);

    try {
        ValidateFlags();
        const auto operation = ParseOperation();
        const auto routing_mode = ParseRoutingMode();
        gRunNamespace =
            FLAGS_key_prefix + "_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count());

        std::vector<std::unique_ptr<SegmentClient>> segment_clients;
        std::mutex segment_clients_mutex;
        std::jthread ping_thread([&](std::stop_token stop_token) {
            static const auto one_second = std::chrono::seconds(1);
            unset_cpu_affinity();
            while (!stop_token.stop_requested()) {
                const auto started_at = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> guard(segment_clients_mutex);
                    for (auto& segment_client : segment_clients) {
                        segment_client->Ping();
                    }
                }
                const auto elapsed =
                    std::chrono::steady_clock::now() - started_at;
                if (elapsed < one_second) {
                    std::this_thread::sleep_for(one_second - elapsed);
                }
            }
        });

        LOG(INFO) << "Mounting " << FLAGS_num_segments << " segments";
        for (uint64_t i = 0; i < FLAGS_num_segments; ++i) {
            auto segment_client = std::make_unique<SegmentClient>(
                "batch_put_completion_segment_" + std::to_string(i),
                FLAGS_master_server, kSegmentBase + i * FLAGS_segment_size,
                FLAGS_segment_size);
            std::lock_guard<std::mutex> guard(segment_clients_mutex);
            segment_clients.push_back(std::move(segment_client));
        }

        const auto slice_lengths = MakeSliceLengths();
        std::atomic<uint64_t> next_key_id{0};
        std::vector<WorkerSummary> worker_summaries(FLAGS_workers);
        std::vector<std::thread> workers;
        workers.reserve(FLAGS_workers);
        auto start_barrier = std::make_shared<std::latch>(FLAGS_workers + 1);

        for (uint64_t worker_idx = 0; worker_idx < FLAGS_workers;
             ++worker_idx) {
            workers.emplace_back([&, worker_idx] {
                unset_cpu_affinity();
                mooncake::MasterClient client(mooncake::generate_uuid());
                auto ec = client.Connect(FLAGS_master_server);
                if (ec != mooncake::ErrorCode::OK) {
                    throw std::runtime_error(
                        "Benchmark worker cannot connect, ec=" +
                        std::string(toString(ec)));
                }

                uint64_t batch_seq = 0;
                auto& summary = worker_summaries[worker_idx];
                summary.latency_us.reserve(FLAGS_requests_per_worker);

                for (uint64_t i = 0; i < FLAGS_warmup_requests; ++i) {
                    auto fixture = PrepareBatch(
                        client, slice_lengths, routing_mode, worker_idx,
                        batch_seq++, next_key_id, summary.prepare_retries);
                    RunCompletion(client, fixture.keys, operation);
                    if (operation == CompletionOperation::kBatchPutEnd) {
                        CleanupCompletedBatch(client, fixture.keys);
                    }
                    ++summary.warmup_batches;
                }

                std::vector<BatchFixture> prepared_batches;
                prepared_batches.reserve(FLAGS_requests_per_worker);
                for (uint64_t i = 0; i < FLAGS_requests_per_worker; ++i) {
                    prepared_batches.push_back(PrepareBatch(
                        client, slice_lengths, routing_mode, worker_idx,
                        batch_seq++, next_key_id, summary.prepare_retries));
                }

                start_barrier->arrive_and_wait();

                for (const auto& batch : prepared_batches) {
                    summary.latency_us.push_back(MeasureCompletionLatencyUs(
                        client, batch.keys, operation));
                }

                if (operation == CompletionOperation::kBatchPutEnd) {
                    for (const auto& batch : prepared_batches) {
                        CleanupCompletedBatch(client, batch.keys);
                    }
                }
            });
        }

        start_barrier->arrive_and_wait();
        const auto measured_start = std::chrono::steady_clock::now();
        for (auto& worker : workers) {
            worker.join();
        }
        const auto measured_end = std::chrono::steady_clock::now();

        SummaryRow row = BuildSummary(
            worker_summaries,
            std::chrono::duration<double>(measured_end - measured_start)
                .count());
        PrintSummary(row);
        WriteCsv(row);

        if (ping_thread.joinable()) {
            ping_thread.request_stop();
            ping_thread.join();
        }
        segment_clients.clear();
        google::ShutdownGoogleLogging();
        return 0;
    } catch (const std::exception& ex) {
        LOG(ERROR) << "benchmark failed: " << ex.what();
        google::ShutdownGoogleLogging();
        return 1;
    }
}
