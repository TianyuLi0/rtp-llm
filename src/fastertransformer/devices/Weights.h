#pragma once

#include "src/fastertransformer/core/Buffer.h"

#include <optional>
#include <memory>
#include <unordered_map>

namespace fastertransformer {

// These weights should correspond to `maga_transformer/utils/model_weight.py`

struct LayerNormWeights {
    ConstBufferPtr gamma = nullptr;
    ConstBufferPtr beta = nullptr;
    LayerNormWeights() = default;

    LayerNormWeights(ConstBufferPtr& gamma,
                     ConstBufferPtr& beta) :
        gamma(std::move(gamma)),
        beta(std::move(beta)) {}

    LayerNormWeights(BufferPtr& gamma,
                     BufferPtr& beta) :
        gamma(std::move(gamma)),
        beta(std::move(beta)) {}
};

typedef std::shared_ptr<const LayerNormWeights> LayerNormWeightsPtr;

struct DenseWeights {
    ConstBufferPtr kernel = nullptr;
    ConstBufferPtr bias = nullptr;

    DenseWeights() = default;

    DenseWeights(BufferPtr& kernel) : kernel(std::move(kernel)) {};

    DenseWeights(ConstBufferPtr& kernel) : kernel(std::move(kernel)) {};

    DenseWeights(ConstBufferPtr& kernel,
                 ConstBufferPtr& bias) :
                 kernel(std::move(kernel)),
                 bias(std::move(bias)) {};

    DenseWeights(BufferPtr& kernel,
                 BufferPtr& bias) :
                 kernel(std::move(kernel)),
                 bias(std::move(bias)) {};
};

typedef std::shared_ptr<const DenseWeights> DenseWeightsPtr;

struct LoraWeights {
    ConstBufferPtr A;
    ConstBufferPtr B;
    ConstBufferPtr A_scale;
    ConstBufferPtr B_scale;
};

typedef std::unordered_map<std::string, LoraWeights> LoraWeightsMap;

struct AttentionLayerWeights {
    std::shared_ptr<const LayerNormWeights> pre_attention_layernorm;
    std::shared_ptr<const DenseWeights>     qkv_weight;
    std::shared_ptr<const LoraWeightsMap>   query_lora_weights;
    std::shared_ptr<const LayerNormWeights> attention_layernorm;

    std::shared_ptr<const DenseWeights>     output_weight;
    std::shared_ptr<const LoraWeightsMap>   output_lora_weights;

    std::shared_ptr<const DenseWeights>     smoother_weight;
    std::shared_ptr<const DenseWeights>     shift_weight;
};

struct FfnLayerWeights {
    std::shared_ptr<const DenseWeights>     up_weight;
    std::shared_ptr<const DenseWeights>     moe_up_weight;
    std::shared_ptr<const LoraWeightsMap>   up_lora_weights;

    std::shared_ptr<const DenseWeights>     gate_weight;
    std::shared_ptr<const DenseWeights>     moe_gate_weight;
    std::shared_ptr<const LoraWeightsMap>   gate_lora_weights;

    std::shared_ptr<const DenseWeights>     down_weight;
    std::shared_ptr<const DenseWeights>     moe_down_weight;
    std::shared_ptr<const LoraWeightsMap>   down_lora_weights;

    std::shared_ptr<const DenseWeights>     moe_gating_weight;

    std::shared_ptr<const DenseWeights>     smoother_weight;
    ConstBufferPtr                          act_scale;

    // these fields are for Qwen Mode model.
    // See https://github.com/huggingface/transformers/blob/0f67ba1d741d65b07d549daf4ee157609ce4f9c1/src/transformers/models/qwen2_moe/modeling_qwen2_moe.py#L803
    std::shared_ptr<FfnLayerWeights>        shared_expert;
    std::shared_ptr<const DenseWeights>     shared_expert_gate;
};

struct LayerWeights {
    std::shared_ptr<const LayerNormWeights> pre_layernorm;
    AttentionLayerWeights                   self_attention_weights;
    std::shared_ptr<const DenseWeights>     pre_attention_smoother_weight;
    std::shared_ptr<const LayerNormWeights> post_layernorm;
    FfnLayerWeights                         ffn_weights;
    std::shared_ptr<const LayerNormWeights> post_ffn_layernorm;
};

// TODO: This Weights class might be refactor into a complete model description
// which includes more info like norm type, activation type, etc.
struct Weights {
    std::shared_ptr<const DenseWeights>     embedding;
    std::shared_ptr<const DenseWeights>     prefix_encoder_embedding;
    std::shared_ptr<const LayerNormWeights> pre_decoder_layernorm;
    std::shared_ptr<const DenseWeights>     position_encoding;
    std::shared_ptr<const DenseWeights>     token_type_embedding;
    std::vector<LayerWeights>               layers;
    std::shared_ptr<const LayerNormWeights> final_layernorm;
    std::shared_ptr<const DenseWeights>     lm_head;
    std::shared_ptr<const DenseWeights>     medusa_head;
};

using WeightsPtr = std::shared_ptr<const Weights>;

}  // namespace fastertransformer
