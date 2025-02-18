#include "source/common/upstream/ring_hash_lb.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "envoy/config/cluster/v3/cluster.pb.h"

#include "source/common/common/assert.h"
#include "source/common/upstream/load_balancer_impl.h"

#include "absl/container/inlined_vector.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Upstream {

RingHashLoadBalancer::RingHashLoadBalancer(
    const PrioritySet& priority_set, ClusterLbStats& stats, Stats::Scope& scope,
    Runtime::Loader& runtime, Random::RandomGenerator& random,
    OptRef<const envoy::config::cluster::v3::Cluster::RingHashLbConfig> config,
    const envoy::config::cluster::v3::Cluster::CommonLbConfig& common_config)
    : ThreadAwareLoadBalancerBase(priority_set, stats, runtime, random,
                                  PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
                                      common_config, healthy_panic_threshold, 100, 50),
                                  common_config.has_locality_weighted_lb_config()),
      scope_(scope.createScope("ring_hash_lb.")), stats_(generateStats(*scope_)),
      min_ring_size_(config.has_value() ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(
                                              config.ref(), minimum_ring_size, DefaultMinRingSize)
                                        : DefaultMinRingSize),
      max_ring_size_(config.has_value() ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(
                                              config.ref(), maximum_ring_size, DefaultMaxRingSize)
                                        : DefaultMaxRingSize),
      hash_function_(config.has_value()
                         ? config->hash_function()
                         : HashFunction::Cluster_RingHashLbConfig_HashFunction_XX_HASH),
      use_hostname_for_hashing_(
          common_config.has_consistent_hashing_lb_config()
              ? common_config.consistent_hashing_lb_config().use_hostname_for_hashing()
              : false),
      hash_balance_factor_(PROTOBUF_GET_WRAPPED_OR_DEFAULT(
          common_config.consistent_hashing_lb_config(), hash_balance_factor, 0)) {
  // It's important to do any config validation here, rather than deferring to Ring's ctor,
  // because any exceptions thrown here will be caught and handled properly.
  if (min_ring_size_ > max_ring_size_) {
    throw EnvoyException(fmt::format("ring hash: minimum_ring_size ({}) > maximum_ring_size ({})",
                                     min_ring_size_, max_ring_size_));
  }
}

RingHashLoadBalancer::RingHashLoadBalancer(
    const PrioritySet& priority_set, ClusterLbStats& stats, Stats::Scope& scope,
    Runtime::Loader& runtime, Random::RandomGenerator& random, uint32_t healthy_panic_threshold,
    const envoy::extensions::load_balancing_policies::ring_hash::v3::RingHash& config)
    : ThreadAwareLoadBalancerBase(priority_set, stats, runtime, random, healthy_panic_threshold,
                                  config.has_locality_weighted_lb_config()),
      scope_(scope.createScope("ring_hash_lb.")), stats_(generateStats(*scope_)),
      min_ring_size_(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, minimum_ring_size, DefaultMinRingSize)),
      max_ring_size_(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, maximum_ring_size, DefaultMaxRingSize)),
      hash_function_(static_cast<HashFunction>(config.hash_function())),
      use_hostname_for_hashing_(
          config.has_consistent_hashing_lb_config()
              ? config.consistent_hashing_lb_config().use_hostname_for_hashing()
              : config.use_hostname_for_hashing()),
      hash_balance_factor_(config.has_consistent_hashing_lb_config()
                               ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(
                                     config.consistent_hashing_lb_config(), hash_balance_factor, 0)
                               : PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, hash_balance_factor, 0)) {
  // It's important to do any config validation here, rather than deferring to Ring's ctor,
  // because any exceptions thrown here will be caught and handled properly.
  if (min_ring_size_ > max_ring_size_) {
    throw EnvoyException(fmt::format("ring hash: minimum_ring_size ({}) > maximum_ring_size ({})",
                                     min_ring_size_, max_ring_size_));
  }
}

RingHashLoadBalancerStats RingHashLoadBalancer::generateStats(Stats::Scope& scope) {
  return {ALL_RING_HASH_LOAD_BALANCER_STATS(POOL_GAUGE(scope))};
}

HostConstSharedPtr RingHashLoadBalancer::Ring::chooseHost(uint64_t h, uint32_t attempt) const {
  if (ring_.empty()) {
    return nullptr;
  }

  // Ported from https://github.com/RJ/ketama/blob/master/libketama/ketama.c (ketama_get_server)
  // I've generally kept the variable names to make the code easier to compare.
  // NOTE: The algorithm depends on using signed integers for lowp, midp, and highp. Do not
  //       change them!
  int64_t lowp, highp;

  // Algorithm is to shard the indices and lookup the host for a given hash from a shard
  // (instead of a lookup of all hosts). Hence fewer lookups/ accesses and faster execution.
  if (Runtime::runtimeFeatureEnabled("envoy.reloadable_features.shard_ringhash")) {

    // Given a hash 'h', find the shard index by shifting it to right (by `rshift_to_shard`).
    // Shifting right by 64 of a 64-bit unsigned int doesn't seem to work! Hence the split.
    uint64_t shard_index = (h >> (rshift_to_shard - 1)) >> 1;

    // Right shift of a 64-bit unsigned int doesn't work if MSB (bit 63) is set.
    // Below code is needed to compute a right shift of such a number.
    uint64_t h_msb = h & 0x8000000000000000;
    if (h_msb != 0UL) {
      shard_index = (((h & 0x7FFFFFFFFFFFFFFF) >> 1) | 0x4000000000000000) >> (rshift_to_shard - 1);
    }

    // 'lowp' and 'highp' are the lower and upper indices of the shard.
    lowp = ring_shard_[shard_index];
    highp = ring_shard_[shard_index + 1];

  } else {

    lowp = 0;
    highp = ring_.size();
  }

  int64_t midp = 0;
  while (true) {
    midp = (lowp + highp) / 2;

    if (midp == static_cast<int64_t>(ring_.size())) {
      midp = 0;
      break;
    }

    uint64_t midval = ring_[midp].hash_;
    uint64_t midval1 = midp == 0 ? 0 : ring_[midp - 1].hash_;

    if (h <= midval && h > midval1) {
      break;
    }

    if (midval < h) {
      lowp = midp + 1;
    } else {
      highp = midp - 1;
    }

    if (lowp > highp) {
      midp = 0;
      break;
    }
  }

  // If a retry host predicate is being applied, behave as if this host was not in the ring.
  // Note that this does not guarantee a different host: e.g., attempt == ring_.size() or
  // when the offset causes us to select the same host at another location in the ring.
  if (attempt > 0) {
    midp = (midp + attempt) % ring_.size();
  }

  return ring_[midp].host_;
}

using HashFunction = envoy::config::cluster::v3::Cluster::RingHashLbConfig::HashFunction;
RingHashLoadBalancer::Ring::Ring(const NormalizedHostWeightVector& normalized_host_weights,
                                 double min_normalized_weight, uint64_t min_ring_size,
                                 uint64_t max_ring_size, HashFunction hash_function,
                                 bool use_hostname_for_hashing, RingHashLoadBalancerStats& stats)
    : stats_(stats) {
  ENVOY_LOG(trace, "ring hash: building ring");

  // We can't do anything sensible with no hosts.
  if (normalized_host_weights.empty()) {
    return;
  }

  // Scale up the number of hashes per host such that the least-weighted host gets a whole number
  // of hashes on the ring. Other hosts might not end up with whole numbers, and that's fine (the
  // ring-building algorithm below can handle this). This preserves the original implementation's
  // behavior: when weights aren't provided, all hosts should get an equal number of hashes. In
  // the case where this number exceeds the max_ring_size, it's scaled back down to fit.
  const double scale =
      std::min(std::ceil(min_normalized_weight * min_ring_size) / min_normalized_weight,
               static_cast<double>(max_ring_size));

  // Reserve memory for the entire ring up front.
  const uint64_t ring_size = std::ceil(scale);
  ring_.reserve(ring_size);

  // Populate the hash ring by walking through the (host, weight) pairs in
  // normalized_host_weights, and generating (scale * weight) hashes for each host. Since these
  // aren't necessarily whole numbers, we maintain running sums -- current_hashes and
  // target_hashes -- which allows us to populate the ring in a mostly stable way.
  //
  // For example, suppose we have 4 hosts, each with a normalized weight of 0.25, and a scale of
  // 6.0 (because the max_ring_size is 6). That means we want to generate 1.5 hashes per host.
  // We start the outer loop with current_hashes = 0 and target_hashes = 0.
  //   - For the first host, we set target_hashes = 1.5. After one run of the inner loop,
  //     current_hashes = 1. After another run, current_hashes = 2, so the inner loop ends.
  //   - For the second host, target_hashes becomes 3.0, and current_hashes is 2 from before.
  //     After only one run of the inner loop, current_hashes = 3, so the inner loop ends.
  //   - Likewise, the third host gets two hashes, and the fourth host gets one hash.
  //
  // For stats reporting, keep track of the minimum and maximum actual number of hashes per host.
  // Users should hopefully pay attention to these numbers and alert if min_hashes_per_host is too
  // low, since that implies an inaccurate request distribution.

  absl::InlinedVector<char, 196> hash_key_buffer;
  double current_hashes = 0.0;
  double target_hashes = 0.0;
  uint64_t min_hashes_per_host = ring_size;
  uint64_t max_hashes_per_host = 0;
  for (const auto& entry : normalized_host_weights) {
    const auto& host = entry.first;
    const absl::string_view key_to_hash = hashKey(host, use_hostname_for_hashing);
    ASSERT(!key_to_hash.empty());

    hash_key_buffer.assign(key_to_hash.begin(), key_to_hash.end());
    hash_key_buffer.emplace_back('_');
    auto offset_start = hash_key_buffer.end();

    // As noted above: maintain current_hashes and target_hashes as running sums across the entire
    // host set. `i` is needed only to construct the hash key, and tally min/max hashes per host.
    target_hashes += scale * entry.second;
    uint64_t i = 0;
    while (current_hashes < target_hashes) {
      const std::string i_str = absl::StrCat("", i);
      hash_key_buffer.insert(offset_start, i_str.begin(), i_str.end());

      absl::string_view hash_key(static_cast<char*>(hash_key_buffer.data()),
                                 hash_key_buffer.size());

      const uint64_t hash =
          (hash_function == HashFunction::Cluster_RingHashLbConfig_HashFunction_MURMUR_HASH_2)
              ? MurmurHash::murmurHash2(hash_key, MurmurHash::STD_HASH_SEED)
              : HashUtil::xxHash64(hash_key);

      ENVOY_LOG(trace, "ring hash: hash_key={} hash={}", hash_key, hash);
      ring_.push_back({hash, host});
      ++i;
      ++current_hashes;
      hash_key_buffer.erase(offset_start, hash_key_buffer.end());
    }
    min_hashes_per_host = std::min(i, min_hashes_per_host);
    max_hashes_per_host = std::max(i, max_hashes_per_host);
  }

  std::sort(ring_.begin(), ring_.end(), [](const RingEntry& lhs, const RingEntry& rhs) -> bool {
    return lhs.hash_ < rhs.hash_;
  });
  if (ENVOY_LOG_CHECK_LEVEL(trace)) {
    for (const auto& entry : ring_) {
      const absl::string_view key_to_hash = hashKey(entry.host_, use_hostname_for_hashing);
      ENVOY_LOG(trace, "ring hash: host={} hash={}", key_to_hash, entry.hash_);
    }
  }

  if (Runtime::runtimeFeatureEnabled("envoy.reloadable_features.shard_ringhash")) {
    /******* BEGIN: code for sharding algorithm *******/

    // Find MSB bit of the first hash so we can right shift all the other hashes to create shards.
    int msb = 0;
    uint64_t n = ring_[0].hash_;
    n = n / 2;
    while (n != 0) {
      n = n / 2;
      msb++;
    }
    // Arbitrarily choose MSB + 'N' bits to shift hash to right for creating shards. A larger shift
    // to right creates fewer shards. Experiment w values 9, 10,... for `rshift_to_shard` parameter.
    rshift_to_shard = ((rshift_to_shard + msb) > 64) ? 64 : (rshift_to_shard + msb);

    // Reserve memory for shard indices. Worst-case, every hash belongs to a different shard!
    // The ring_shard_ container stores the start indices of the shards.
    ring_shard_.reserve(ring_size);

    // Right shift each hash and create shards of the hosts.
    uint64_t ring_index = 0;
    uint64_t current_shard, prev_shard = 0;

    // push the first index if the ring_ isn't empty
    if (!ring_.empty()) {
      ring_shard_.push_back(ring_index);
      ring_index++;
    }

    for (const auto& entry : ring_) {
      // Shifting right by 64 of a 64-bit unsigned int doesn't seem to work! Hence the split.
      current_shard = (entry.hash_ >> (rshift_to_shard - 1)) >> 1;

      // If new shard found, push the index to ring_shard_ and update current_shard.
      if (current_shard != prev_shard) {
        prev_shard = current_shard;
        ring_shard_.push_back(ring_index);
      }
      ring_index++;
    }
    // For the last shard, we need end and hence storing ring_size value.
    ring_shard_.push_back(ring_size);

    /******* END: code for sharding algorithm *******/
  }

  stats_.size_.set(ring_size);
  stats_.min_hashes_per_host_.set(min_hashes_per_host);
  stats_.max_hashes_per_host_.set(max_hashes_per_host);
}

} // namespace Upstream
} // namespace Envoy
