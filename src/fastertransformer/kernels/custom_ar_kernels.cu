/*
 * Copyright (c) 2022-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "custom_ar_kernels.h"
#include "src/fastertransformer/cuda/cuda_type_utils.cuh"
#include <cassert>
#if USING_ROCM
#include "src/fastertransformer/rocm/cuda_shims.h"
#endif
#include <cstddef>

namespace fastertransformer {

////////////////////////////////////////////////////////////////////////////////////////////////////

static inline __device__ uint32_t hadd2(const uint32_t& a, const uint32_t& b) {
#if USING_ROCM
    __half2 out = __hadd2(*reinterpret_cast<const __half2_raw*>(&a), *reinterpret_cast<const __half2_raw*>(&b));
    return *reinterpret_cast<uint32_t*>(&(out.data));
#else
    uint32_t c;
    asm volatile("add.f16x2 %0, %1, %2;\n" : "=r"(c) : "r"(a), "r"(b));
    return c;
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////

static inline __device__ uint32_t fadd(const uint32_t& a, const uint32_t& b) {
    uint32_t c;
#if USING_ROCM
    c = __float_as_uint(__uint_as_float(a) + __uint_as_float(b));
#else
    asm volatile("add.f32 %0, %1, %2;\n" : "=r"(c) : "r"(a), "r"(b));
#endif
    return c;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

static inline __device__ void st_flag_release(uint32_t& flag, uint32_t* flag_addr) {
#if USING_ROCM
    __atomic_store((__attribute__((address_space(1))) uint32_t*)flag_addr,
                   (__attribute__((address_space(1))) uint32_t*)&flag,
                   __ATOMIC_RELEASE);
#else
#if __CUDA_ARCH__ >= 700
    asm volatile("st.global.release.sys.b32 [%1], %0;" ::"r"(flag), "l"(flag_addr));
#else
    __threadfence_system();
    asm volatile("st.global.volatile.b32 [%1], %0;" ::"r"(flag), "l"(flag_addr));
#endif
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////

static inline __device__ void ld_flag_acquire(uint32_t& flag, uint32_t* flag_addr) {
#if USING_ROCM
    __atomic_load((__attribute__((address_space(1))) uint32_t*)flag_addr, &flag, __ATOMIC_ACQUIRE);
#else
#if __CUDA_ARCH__ >= 700
    asm volatile("ld.global.acquire.sys.b32 %0, [%1];" : "=r"(flag) : "l"(flag_addr));
#else
    asm volatile("ld.global.volatile.b32 %0, [%1];" : "=r"(flag) : "l"(flag_addr));
#endif
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// Type Converter that packs data format to 128 bits data type
template<typename T>
struct ARTypeConverter {
    using Type = uint4;
};

template<>
struct ARTypeConverter<__nv_bfloat16> {
    using Type = bf168;
};

// add two 128b data
template<typename T_IN, typename T_COMP>
inline __device__ T_IN add128b(T_IN a, T_IN b);

template<>
inline __device__ uint4 add128b<uint4, half>(uint4 a, uint4 b) {
    uint4 c;
    c.x = hadd2(a.x, b.x);
    c.y = hadd2(a.y, b.y);
    c.z = hadd2(a.z, b.z);
    c.w = hadd2(a.w, b.w);
    return c;
}

template<>
inline __device__ uint4 add128b<uint4, float>(uint4 a, uint4 b) {
    uint4 c;
    c.x = fadd(a.x, b.x);
    c.y = fadd(a.y, b.y);
    c.z = fadd(a.z, b.z);
    c.w = fadd(a.w, b.w);
    return c;
}

#ifdef ENABLE_BF16
template<>
inline __device__ bf168 add128b<bf168, __nv_bfloat16>(bf168 a, bf168 b) {
    bf168 c;
    c.x = bf16hadd2(a.x, b.x);
    c.y = bf16hadd2(a.y, b.y);
    c.z = bf16hadd2(a.z, b.z);
    c.w = bf16hadd2(a.w, b.w);
    return c;
}
#endif

// init 128bits data with 0
template<typename T>
inline __device__ T init_packed_type();

template<>
inline __device__ uint4 init_packed_type() {
    return make_uint4(0u, 0u, 0u, 0u);
}

template<>
inline __device__ bf168 init_packed_type() {
    bf168  val;
    uint4& val_u = reinterpret_cast<uint4&>(val);
    val_u        = make_uint4(0u, 0u, 0u, 0u);
    return val;
}

template<typename T, size_t RANKS_PER_NODE>
static __global__ void oneShotAllReduceKernel(CustomAllReduceParameters params) {
    // The block index.
    const size_t bidx = blockIdx.x;
    // The thread index with the block.
    const size_t tidx = threadIdx.x;

    // The number of elements packed into one for comms
    static constexpr size_t NUM_ELTS = std::is_same<T, float>::value ? 4 : 8;

    // Packed data type for comms
    using PackedType = typename ARTypeConverter<T>::Type;

    // The location in the destination array (load 8 fp16 or load 4 fp32 using LDG.128).
    size_t offset = bidx * params.elts_per_block + tidx * NUM_ELTS;
    // The end of the segment computed by that block.
    size_t max_offset = std::min((bidx + 1) * params.elts_per_block, params.elts_per_rank);

    // Synchronize the ranks.
    if (tidx < RANKS_PER_NODE) {
        // The 1st block notifies the other ranks.
        if (bidx == 0) {
            st_flag_release(params.barrier_flag, params.peer_barrier_ptrs[tidx] + params.local_rank);
        }
        uint32_t* peer_barrier_d = params.peer_barrier_ptrs[params.local_rank] + tidx;
        uint32_t  rank_barrier   = 0;
        // Busy-wait until all ranks are ready.
        do {
            ld_flag_acquire(rank_barrier, peer_barrier_d);
        } while (rank_barrier != params.barrier_flag);
    }

    // Make sure we can move on...
    __syncthreads();

    // The source pointers. Distributed round-robin for the different warps.
    const T* src_d[RANKS_PER_NODE];
#pragma unroll
    for (size_t ii = 0; ii < RANKS_PER_NODE; ++ii) {
        size_t rank = (params.local_rank + ii) % RANKS_PER_NODE;
        src_d[ii]   = (T*)(params.peer_comm_buffer_ptrs[rank]);
    }

    // Each block accumulates the values from the different GPUs on the same node.
    for (size_t iter_offset = offset; iter_offset < max_offset; iter_offset += blockDim.x * NUM_ELTS) {
        // Iterate over the different ranks/devices on the node to load the values.
        PackedType vals[RANKS_PER_NODE];
#pragma unroll
        for (size_t ii = 0; ii < RANKS_PER_NODE; ++ii) {
            vals[ii] = reinterpret_cast<const PackedType*>(&src_d[ii][iter_offset])[0];
        }

        // Sum the values from the different ranks.
        PackedType sums = init_packed_type<PackedType>();
#pragma unroll
        for (size_t ii = 0; ii < RANKS_PER_NODE; ++ii) {
            sums = add128b<PackedType, T>(sums, vals[ii]);
        }

        // Store to the destination buffer.
        reinterpret_cast<PackedType*>(&((T*)params.local_output_buffer_ptr)[iter_offset])[0] = sums;
    }
}

template<typename T, size_t RANKS_PER_NODE>
static __global__ void twoShotAllReduceKernel(CustomAllReduceParameters params) {

    // The block index.
    const size_t bidx = blockIdx.x;
    // The thread index with the block.
    const size_t tidx = threadIdx.x;

    // The number of elements packed into one for comms
    static constexpr size_t NUM_ELTS = std::is_same<T, float>::value ? 4 : 8;

    // Packed data type for comms
    using PackedType = typename ARTypeConverter<T>::Type;

    // The location in the destination array (load 8 fp16 or load 4 fp32 using LDG.128).
    size_t offset = bidx * params.elts_per_block + tidx * NUM_ELTS + params.rank_offset;
    // The end of the segment computed by that block.
    size_t max_offset = min(offset + params.elts_per_block, params.elts_total_num);

    // Synchronize the ranks.

    if (tidx < RANKS_PER_NODE) {
        // The 1st block notifies the other ranks.
        if (bidx == 0) {
            st_flag_release(params.barrier_flag, params.peer_barrier_ptrs[tidx] + params.local_rank);
        }
        uint32_t* peer_barrier_d = params.peer_barrier_ptrs[params.local_rank] + tidx;
        uint32_t  rank_barrier   = 0;
        // Busy-wait until all ranks are ready.
        do {
            ld_flag_acquire(rank_barrier, peer_barrier_d);
        } while (rank_barrier != params.barrier_flag);
    }

    // Make sure we can move on...
    __syncthreads();

    // The source pointers. Distributed round-robin for the different warps.
    T* src_d[RANKS_PER_NODE];
    // The destination ranks for round-robin gathering
    size_t dst_rank[RANKS_PER_NODE];
#pragma unroll
    for (size_t ii = 0; ii < RANKS_PER_NODE; ++ii) {
        size_t rank  = (params.local_rank + ii) % RANKS_PER_NODE;
        src_d[ii]    = (T*)(params.peer_comm_buffer_ptrs[rank]);
        dst_rank[ii] = rank;
    }

    // Each block accumulates the values from the different GPUs on the same node.
    for (size_t local_offset = offset; local_offset < max_offset; local_offset += blockDim.x * NUM_ELTS) {

        // Iterate over the different ranks/devices on the node to load the values.
        PackedType vals[RANKS_PER_NODE];
#pragma unroll
        for (size_t ii = 0; ii < RANKS_PER_NODE; ++ii) {
            vals[ii] = reinterpret_cast<const PackedType*>(&src_d[ii][local_offset])[0];
        }

        // Sum the values from the different ranks.
        PackedType sums = init_packed_type<PackedType>();
#pragma unroll
        for (size_t ii = 0; ii < RANKS_PER_NODE; ++ii) {
            sums = add128b<PackedType, T>(sums, vals[ii]);
        }

        // Store to the local buffer.
        reinterpret_cast<PackedType*>(&src_d[0][local_offset])[0] = sums;
    }

    // sync threads to make sure all block threads have the sums
    __syncthreads();

    // barreris among the blocks with the same idx (release-acuqire semantics)
    if (tidx < RANKS_PER_NODE) {
        // The all blocks notifies the other ranks.
        uint32_t flag_block_offset = RANKS_PER_NODE + bidx * RANKS_PER_NODE;
        st_flag_release(params.barrier_flag, params.peer_barrier_ptrs[tidx] + flag_block_offset + params.local_rank);

        // Busy-wait until all ranks are ready.
        uint32_t  rank_barrier   = 0;
        uint32_t* peer_barrier_d = params.peer_barrier_ptrs[params.local_rank] + flag_block_offset + tidx;
        do {
            ld_flag_acquire(rank_barrier, peer_barrier_d);
        } while (rank_barrier != params.barrier_flag);
    }

    // sync threads to make sure all other ranks has the final partial results
    __syncthreads();

    // Gather all needed elts_num from other intra-node ranks
    for (size_t local_offset = offset; local_offset < max_offset; local_offset += blockDim.x * NUM_ELTS) {
#pragma unroll
        for (size_t ii = 0; ii < RANKS_PER_NODE; ++ii) {
            // use round-robin gathering from other ranks
            size_t offset_rank = local_offset + (dst_rank[ii] - params.local_rank) * params.elts_per_rank;
            reinterpret_cast<PackedType*>(&((T*)params.local_output_buffer_ptr)[offset_rank])[0] =
                reinterpret_cast<PackedType*>(&src_d[ii][offset_rank])[0];
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void kernelLaunchConfig(CustomAllReduceParameters* param, size_t& blocks_per_grid, size_t& threads_per_block) {
    size_t data_type_bytes = param->data_type_size;
    assert(data_type_bytes == 2 || data_type_bytes == 4);

    size_t                ranks_per_node = param->ranks_per_node;
    AllReduceStrategyType kernel_algo    = param->kernel_algo;

    size_t elts_total_num  = param->elts_total_num;
    size_t elts_per_thread = 16 / data_type_bytes;
    switch (kernel_algo) {
        case AllReduceStrategyType::ONESHOT: {  // one stage all reduce algo
            assert(elts_total_num % elts_per_thread == 0);
            size_t const total_threads = roundUp(elts_total_num / elts_per_thread, WARP_SIZE);
            threads_per_block          = std::min((size_t)DEFAULT_BLOCK_SIZE, total_threads);
            blocks_per_grid =
                std::min(static_cast<size_t>(MAX_ALL_REDUCE_BLOCKS), divUp(total_threads, threads_per_block));
            param->elts_per_rank  = elts_total_num;
            param->rank_offset    = 0;
            param->elts_per_block = roundUp(divUp(elts_total_num, blocks_per_grid), elts_per_thread);

            // std::stringstream string_stream;
            // string_stream  << "[ONE] elts_total_num: " << elts_total_num << " blocks_per_grid: " << blocks_per_grid << " total_threads: " << total_threads << " threads_per_block: " << threads_per_block << " elts_per_thread: " << elts_per_thread << std::endl; FT_LOG_INFO(string_stream.str());
            // FT_LOG_INFO(string_stream.str());
            break;
        }
        case AllReduceStrategyType::TWOSHOT: {  // two stage all reduce algo
            size_t mod    = elts_per_thread * ranks_per_node * DEFAULT_BLOCK_SIZE * MAX_ALL_REDUCE_BLOCKS;
            size_t remain = param->elts_total_num % mod;
            bool half_mod = false;
            if (remain != 0) {
                size_t max_elts_num = param->max_elts_total_size / data_type_bytes;
                assert(max_elts_num % mod == 0);
                if (elts_total_num < mod * 2) {
                    mod = elts_per_thread * ranks_per_node * DEFAULT_BLOCK_SIZE * MAX_ALL_REDUCE_BLOCKS / 2;
                    remain = param->elts_total_num % mod;
                    half_mod = true;
                }
                elts_total_num        += (mod - remain);
                elts_total_num        = std::min(elts_total_num, max_elts_num);
                param->elts_total_num = elts_total_num;
            }

            assert(elts_total_num / (elts_per_thread * ranks_per_node) == 0);
            size_t const total_threads = elts_total_num / (elts_per_thread * ranks_per_node);

            threads_per_block = DEFAULT_BLOCK_SIZE;
            blocks_per_grid   = MAX_ALL_REDUCE_BLOCKS;
            if (half_mod) {
                blocks_per_grid = MAX_ALL_REDUCE_BLOCKS / 2;
            }

            param->elts_per_rank  = elts_total_num / ranks_per_node;
            param->rank_offset    = param->rank * param->elts_per_rank;
            param->elts_per_block = roundUp(divUp(param->elts_per_rank, blocks_per_grid), elts_per_thread);

            // std::stringstream string_stream;
            // string_stream  << "[TWO] elts_total_num: " << elts_total_num << " blocks_per_grid: " << blocks_per_grid << " total_threads: " << total_threads << " threads_per_block: " << threads_per_block << " elts_per_thread: " << elts_per_thread << std::endl; FT_LOG_INFO(string_stream.str());
            // FT_LOG_INFO(string_stream.str());
            break;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

template<typename T, size_t RANKS_PER_NODE>
void invokeCustomAllReduceKernel(CustomAllReduceParameters* param, cudaStream_t stream) {
    size_t blocks_per_grid = 1, threads_per_block = DEFAULT_BLOCK_SIZE;

    kernelLaunchConfig(param, blocks_per_grid, threads_per_block);

    if (param->kernel_algo == 0) {
        oneShotAllReduceKernel<T, RANKS_PER_NODE><<<blocks_per_grid, threads_per_block, 0, stream>>>(*param);
    } else {
        twoShotAllReduceKernel<T, RANKS_PER_NODE><<<blocks_per_grid, threads_per_block, 0, stream>>>(*param);
    }
}

template<typename T>
void invokeCustomAllReduceDispatch(CustomAllReduceParameters* param, cudaStream_t stream) {
    switch (param->elts_per_rank) {
        case 2:
            invokeCustomAllReduceKernel<T, 2>(param, stream);
            break;
        case 4:
            invokeCustomAllReduceKernel<T, 4>(param, stream);
            break;
        case 8:
            invokeCustomAllReduceKernel<T, 8>(param, stream);
            break;
        default:
            break;
    }
}

#define INSTANTIATE_GENERAL_CUSTOM_ALL_REDUCE_DISPATCH(T)                                                              \
    template void invokeCustomAllReduceDispatch<T>(CustomAllReduceParameters * param, cudaStream_t stream);

#define INSTANTIATE_GENERAL_CUSTOM_ALL_REDUCE_KERNEL(T)                                                                \
    template void invokeCustomAllReduceKernel<T, 2>(CustomAllReduceParameters * param, cudaStream_t stream);           \
    template void invokeCustomAllReduceKernel<T, 4>(CustomAllReduceParameters * param, cudaStream_t stream);           \
    template void invokeCustomAllReduceKernel<T, 8>(CustomAllReduceParameters * param, cudaStream_t stream);

// Template instantiation

#ifdef ENABLE_BF16
INSTANTIATE_GENERAL_CUSTOM_ALL_REDUCE_DISPATCH(__nv_bfloat16)
#endif
INSTANTIATE_GENERAL_CUSTOM_ALL_REDUCE_DISPATCH(float)
INSTANTIATE_GENERAL_CUSTOM_ALL_REDUCE_DISPATCH(half)

#ifdef ENABLE_BF16
INSTANTIATE_GENERAL_CUSTOM_ALL_REDUCE_KERNEL(__nv_bfloat16)
#endif
INSTANTIATE_GENERAL_CUSTOM_ALL_REDUCE_KERNEL(float)
INSTANTIATE_GENERAL_CUSTOM_ALL_REDUCE_KERNEL(half)

}  // namespace fastertransformer
