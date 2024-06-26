#include <bitset>
#include <cstdint>
#include <nvbench/nvbench.hpp>
#include <thrust/binary_search.h>
#include <thrust/device_vector.h>
#include <thrust/execution_policy.h>
#include <thrust/fill.h>
#include <thrust/host_vector.h>
#include <thrust/sequence.h>
#ifdef USE_HIP
#include <thrust/system/hip/detail/execution_policy.h>
#else
#include <cuda.h>
#include <thrust/system/cuda/detail/execution_policy.h>
#endif

__device__ __host__ int popcount(std::uint32_t bitmask) {
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
  return __popc(bitmask);
#else
  std::bitset<32> bits{bitmask};
  return bits.count();
#endif
}

__device__ __host__ int popcount(std::uint64_t bitmask) {
#if defined(__CUDA_ARCH__) || defined(__HIP_DEVICE_COMPILE__)
  return __popcll(bitmask);
#else
  std::bitset<64> bits{bitmask};
  return bits.count();
#endif
}

template <typename T1, typename T2> constexpr auto ceildiv(T1 a, T2 b) {
  return (a + b - 1) / b;
}

template <typename MaskType> __device__ MaskType prefix_mask(int lane) {
  return (MaskType{1} << lane) - 1;
}

const auto sizes = nvbench::range<int, std::int64_t>(16, 28, 2);
const auto threadblocksizes = std::vector<nvbench::int64_t>{512};

using mask_types = nvbench::type_list<std::uint32_t, std::uint64_t>;
using rank_types = nvbench::type_list<std::int32_t, std::int64_t>;

//

template <typename RankType>
__global__ void fill(RankType *__restrict__ out, int size) {
  const auto i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i < size) {
    out[i] = 1;
  }
}

template <typename RankType>
void fill_baseline(nvbench::state &state, nvbench::type_list<RankType>) {
  const auto size = static_cast<std::size_t>(state.get_int64("size"));
  const auto threadblocksize = state.get_int64("threadblocksize");

  thrust::device_vector<RankType> output(size, 0);
  const auto num_threadblocks = ceildiv(size, threadblocksize);

  state.add_element_count(size, "Items");
  state.add_global_memory_writes<RankType>(size, "OutSize");

  state.exec([&](nvbench::launch &launch) {
    fill<<<num_threadblocks, threadblocksize, 0, launch.get_stream()>>>(
        thrust::raw_pointer_cast(output.data()), size);
  });
}

NVBENCH_BENCH_TYPES(fill_baseline, NVBENCH_TYPE_AXES(rank_types))
    .add_int64_axis("threadblocksize", threadblocksizes)
    .add_int64_power_of_two_axis("size", sizes);

//

template <typename MaskType, typename RankType>
__global__ void copy(const MaskType *__restrict__ masks,
                     RankType *__restrict__ out, int size) {
  const auto i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i < size) {
    out[i] = masks[i];
  }
}

template <typename RankType>
void copy_baseline(nvbench::state &state, nvbench::type_list<RankType>) {
  const auto size = static_cast<std::size_t>(state.get_int64("size"));
  const auto threadblocksize = state.get_int64("threadblocksize");

  thrust::device_vector<RankType> input(size, 0);
  thrust::device_vector<RankType> output(size, 0);
  const auto num_threadblocks = ceildiv(size, threadblocksize);

  state.add_element_count(size, "Items");
  state.add_global_memory_reads<RankType>(size, "InSize");
  state.add_global_memory_writes<RankType>(size, "OutSize");

  state.exec([&](nvbench::launch &launch) {
    copy<<<num_threadblocks, threadblocksize, 0, launch.get_stream()>>>(
        thrust::raw_pointer_cast(input.data()),
        thrust::raw_pointer_cast(output.data()), size);
  });
}

NVBENCH_BENCH_TYPES(copy_baseline, NVBENCH_TYPE_AXES(rank_types))
    .add_int64_axis("threadblocksize", threadblocksizes)
    .add_int64_power_of_two_axis("size", sizes);

//

template <typename MaskType, typename RankType>
__global__ void compute_ranks(const MaskType *__restrict__ masks,
                              const RankType *__restrict__ ranks,
                              RankType *__restrict__ out, int size) {
  const auto i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i < size) {
    constexpr auto block_size = CHAR_BIT * sizeof(MaskType);
    const auto block_i = i / block_size;
    const auto local_i = i % block_size;
    out[i] = ranks[block_i] +
             popcount(masks[block_i] & prefix_mask<MaskType>(local_i));
  }
}

template <typename MaskType, typename RankType>
void rank_operation(nvbench::state &state,
                    nvbench::type_list<MaskType, RankType>) {
  const auto size = static_cast<std::size_t>(state.get_int64("size"));
  const auto threadblocksize = state.get_int64("threadblocksize");

  constexpr auto block_size = CHAR_BIT * sizeof(MaskType);
  const auto block_count = ceildiv(size, block_size);
  thrust::device_vector<MaskType> masks(block_count, ~MaskType{});
  thrust::device_vector<RankType> ranks(block_count, 0);
  thrust::sequence(ranks.begin(), ranks.end(), RankType{});
  thrust::for_each(ranks.begin(), ranks.end(),
                   [] __device__(RankType & rank) { rank *= block_size; });
  thrust::device_vector<RankType> output(size, 0);
  const auto num_threadblocks = ceildiv(size, threadblocksize);

  state.add_element_count(size, "Items");
  state.add_global_memory_reads<MaskType>(block_count, "Masks");
  state.add_global_memory_reads<RankType>(block_count, "Ranks");
  state.add_global_memory_writes<RankType>(size, "OutSize");

  state.exec([&](nvbench::launch &launch) {
    compute_ranks<<<num_threadblocks, threadblocksize, 0,
                    launch.get_stream()>>>(
        thrust::raw_pointer_cast(masks.data()),
        thrust::raw_pointer_cast(ranks.data()),
        thrust::raw_pointer_cast(output.data()), size);
  });
  // compare to reference
  auto ref = thrust::host_vector<RankType>(size);
  thrust::sequence(ref.begin(), ref.end(), RankType{});
  if (ref != output) {
    std::cout << "FAIL\n";
  }
}

NVBENCH_BENCH_TYPES(rank_operation, NVBENCH_TYPE_AXES(mask_types, rank_types))
    .set_type_axes_names({"mask", "rank"})
    .add_int64_axis("threadblocksize", threadblocksizes)
    .add_int64_power_of_two_axis("size", sizes);

//

template <typename RankType>
void binary_search_operation(nvbench::state &state,
                             nvbench::type_list<RankType>) {
  const auto size = static_cast<std::size_t>(state.get_int64("size"));

  thrust::device_vector<RankType> ranks(size, 0);
  thrust::sequence(ranks.begin(), ranks.end(), RankType{});
  auto queries = ranks;
  thrust::device_vector<RankType> output(size, 0);

  state.add_element_count(size, "Items");
  state.add_global_memory_reads<RankType>(size, "Ranks");
  state.add_global_memory_reads<RankType>(size, "Queries");
  state.add_global_memory_writes<RankType>(size, "OutSize");

  state.exec(nvbench::exec_tag::sync, [&](nvbench::launch &launch) {
#ifdef USE_HIP
    auto policy = thrust::hip::par.on(launch.get_stream());
#else
    auto policy = thrust::cuda::par.on(launch.get_stream());
#endif
    thrust::lower_bound(policy, ranks.begin(), ranks.end(), queries.begin(),
                        queries.end(), output.begin());
  });
  if (output != ranks) {
    std::cout << "FAIL\n";
  }
}

NVBENCH_BENCH_TYPES(binary_search_operation, NVBENCH_TYPE_AXES(rank_types))
    .add_int64_power_of_two_axis("size", sizes);

//

template <typename MaskType>
__global__ void compute_select(const MaskType *__restrict__ masks,
                               int *__restrict__ out, int size) {
  const auto i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i < size) {
    constexpr auto block_size = CHAR_BIT * sizeof(MaskType);
    int offset = 0;
    const auto mask = masks[i / block_size];
    const auto rank = threadIdx.x % block_size;
    for (int range_size = block_size; range_size > 1; range_size /= 2) {
      const auto mid = offset + range_size / 2;
      const auto half_count = popcount(mask & prefix_mask<MaskType>(mid));
      offset = half_count <= rank ? mid : offset;
    }
    out[i] = offset;
  }
}

template <typename MaskType>
void select_operation(nvbench::state &state, nvbench::type_list<MaskType>) {
  const auto size = static_cast<std::size_t>(state.get_int64("size"));
  const auto threadblocksize = state.get_int64("threadblocksize");

  constexpr auto block_size = CHAR_BIT * sizeof(MaskType);
  const auto block_count = ceildiv(size, block_size);
  thrust::device_vector<MaskType> masks(block_count, ~MaskType{});
  thrust::device_vector<int> output(size, 0);
  const auto num_threadblocks = ceildiv(size, threadblocksize);

  state.add_element_count(size, "Items");
  state.add_global_memory_reads<MaskType>(block_count, "Masks");
  state.add_global_memory_writes<int>(size, "OutSize");

  state.exec([&](nvbench::launch &launch) {
    compute_select<<<num_threadblocks, threadblocksize, 0,
                     launch.get_stream()>>>(
        thrust::raw_pointer_cast(masks.data()),
        thrust::raw_pointer_cast(output.data()), size);
  });
  auto ref = thrust::host_vector<int>(size);
  thrust::sequence(ref.begin(), ref.end(), 0);
  thrust::for_each(ref.begin(), ref.end(),
                   [](int &rank) { rank %= block_size; });
  if (ref != output) {
    std::cout << "FAIL\n";
    for (int i = 0; i < 50; i++) {
      std::cout << ref[i] << ' ' << output[i] << '\n';
    }
  }
}

NVBENCH_BENCH_TYPES(select_operation, NVBENCH_TYPE_AXES(mask_types))
    .add_int64_axis("threadblocksize", threadblocksizes)
    .add_int64_power_of_two_axis("size", sizes);

//

template <typename MaskType>
__global__ void compute_select_even(const MaskType *__restrict__ masks,
                                    int *__restrict__ out, int size) {
  const auto i = threadIdx.x + blockIdx.x * blockDim.x;
  if (i < size) {
    constexpr auto block_size = CHAR_BIT * sizeof(MaskType);
    int offset = 0;
    const auto mask = masks[i / (block_size / 2)];
    const auto rank = threadIdx.x % (block_size / 2);
    for (int range_size = block_size; range_size > 1; range_size /= 2) {
      const auto mid = offset + range_size / 2;
      const auto half_count = popcount(mask & prefix_mask<MaskType>(mid));
      offset = half_count <= rank ? mid : offset;
    }
    out[i] = offset;
  }
}

template <typename MaskType>
void select_even_operation(nvbench::state &state,
                           nvbench::type_list<MaskType>) {
  // Allocate input data:
  const auto size = static_cast<std::size_t>(state.get_int64("size"));
  const auto threadblocksize = state.get_int64("threadblocksize");

  constexpr auto block_size = CHAR_BIT * sizeof(MaskType);
  const auto block_count = ceildiv(size, block_size);
  MaskType mask{};
  for (int i = 0; i < block_size; i += 2) {
    mask |= MaskType{1} << i;
  }
  thrust::device_vector<MaskType> masks(block_count, mask);
  thrust::device_vector<int> output(size / 2, 0);
  const auto num_threadblocks = ceildiv(size / 2, threadblocksize);

  state.add_element_count(size, "Items");
  state.add_global_memory_reads<MaskType>(block_count, "Masks");
  state.add_global_memory_writes<int>(size / 2, "OutSize");

  state.exec([&](nvbench::launch &launch) {
    compute_select_even<<<num_threadblocks, threadblocksize, 0,
                          launch.get_stream()>>>(
        thrust::raw_pointer_cast(masks.data()),
        thrust::raw_pointer_cast(output.data()), size / 2);
  });
  auto ref = thrust::host_vector<int>(size / 2);
  thrust::sequence(ref.begin(), ref.end(), 0);
  thrust::for_each(ref.begin(), ref.end(),
                   [](int &rank) { rank = (rank % (block_size / 2)) * 2; });
  if (ref != output) {
    std::cout << "FAIL\n";
    for (int i = 0; i < 50; i++) {
      std::cout << ref[i] << ' ' << output[i] << '\n';
    }
  }
}

NVBENCH_BENCH_TYPES(select_even_operation, NVBENCH_TYPE_AXES(mask_types))
    .add_int64_axis("threadblocksize", threadblocksizes)
    .add_int64_power_of_two_axis("size", sizes);
