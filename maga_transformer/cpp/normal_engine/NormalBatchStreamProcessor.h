#pragma once

#include "src/fastertransformer/devices/DeviceBase.h"
#include "src/fastertransformer/th_op/GptInitParameter.h"
#include "maga_transformer/cpp/dataclass/MergedQuery.h"
#include "maga_transformer/cpp/dataclass/StreamGroups.h"
#include "absl/status/statusor.h"
#include "absl/status/status.h"

namespace rtp_llm {

class NormalBatchStreamProcessor {
public:
    NormalBatchStreamProcessor(const GptInitParameter& params):
        num_layers_(params.num_layers_),
        use_int8_kv_cache_(params.int8_kv_cache_),
        device_(ft::DeviceFactory::getDevice(ft::DeviceType::Cuda)) {}
    absl::Status                   dispatch(const StreamGroups&                  stream_groups,
                                            const std::unique_ptr<MergedOutput>& merge_outputs) const;
    absl::StatusOr<GptModelInputs> gatherModelInput(const StreamGroups& stream_groups) const;
    absl::StatusOr<SamplerInputs>  gatherSamplerInput(const StreamGroups&    stream_groups,
                                                      const GptModelOutputs& model_output) const;

private:
    size_t          num_layers_;
    bool            use_int8_kv_cache_;
    ft::DeviceBase* device_;
};

}  // namespace rtp_llm