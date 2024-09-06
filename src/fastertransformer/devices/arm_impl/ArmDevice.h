#pragma once

#include "src/fastertransformer/devices/DeviceBase.h"

#include "arm_compute/runtime/NEON/NEFunctions.h"
#include "arm_compute/runtime/Scheduler.h"
#include "gemm_opt/ArmGemmKernel.h"
#include "src/fastertransformer/devices/utils/Timer.h"

namespace fastertransformer {

class ArmCpuDevice : public DeviceBase {
public:
    ArmCpuDevice(const DeviceInitParams& params);
    ~ArmCpuDevice();

public:
    DeviceProperties getDeviceProperties() override;
    IAllocator* getAllocator() override { return allocator_.get(); }
    IAllocator* getHostAllocator() override { return allocator_.get(); }

public:
    void copy(const CopyParams& params) override;
    LayernormOutput layernorm(const LayernormParams& params) override;
    BufferPtr gemm(const GemmParams& params) override;
    BufferPtr gemm_acl(const GemmParams& params);
    BufferPtr gemm_opt(const GemmParams& params);
    GroupedGemmOutput groupedGemm(const GroupedGemmParams& params) override;
    BufferPtr embeddingLookup(const EmbeddingLookupParams& params) override;
    BufferPtr activation(const ActivationParams& params) override;
    BufferPtr softmax(const SoftmaxParams& params) override;
    AttentionModuleOutput contextAttention(const AttentionModuleParams& params) override;
    AttentionModuleOutput decoderSelfAttention(const AttentionModuleParams& params) override;
    void sampleGreedy(const GreedyParams& params) override;
    void sampleBeamSearch(const BeamSearchParams& params) override;
    void broadcast(const BroadcastParams& params) override;
    void allReduceSum(const AllReduceParams& params);
    void printStat();
    DeviceStatus getDeviceStatus();
#ifdef GEMM_DEBUG
    static void print_time();
#endif

private:
    std::unique_ptr<IAllocator> allocator_;
    arm_compute::DataType getAclDataType(DataType type);
    void runOneBatch(const AttentionModuleParams& params, int batch, size_t seq_len, size_t step);
    void runOneBatchStride(const AttentionModuleParams& params, int batch, size_t seq_len, size_t step);
    void contextAttentionStride(const AttentionModuleParams& params);
    void decoderSelfAttentionStride(const AttentionModuleParams& params);
    void contextAttentionFallback(const AttentionModuleParams& params);
    void decoderSelfAttentionFallback(const AttentionModuleParams& params);
    void logTime(std::chrono::microseconds diff, size_t index);
    uint64_t  a_cnt_[16] = {0};
    uint64_t a_tmin_[16] = {999999999, 999999999, 999999999, 999999999, 999999999, 999999999, 999999999, 999999999,
                            999999999, 999999999, 999999999, 999999999, 999999999, 999999999, 999999999, 999999999};
    uint64_t a_tmax_[16] = {0};
    uint64_t a_tave_[16] = {0};
    GemmKernel gemm_kernel_;

#ifdef GEMM_DEBUG
    static TimerRecorder timer_recorder_;
#endif
};

} // namespace fastertransformer

