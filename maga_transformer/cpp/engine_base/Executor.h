#pragma once

#include "absl/status/statusor.h"
#include "maga_transformer/cpp/dataclass/GenerateStream.h"
#include "maga_transformer/cpp/models/GptModel.h"
#include "src/fastertransformer/devices/DeviceBase.h"
#include <memory>

namespace rtp_llm {

class Executor {
public:
    Executor(ft::DeviceBase* device): device_(device){};
    virtual absl::Status
    addLoRA(const int64_t                                                           lora_id,
            const std::vector<std::unordered_map<std::string, ft::ConstBufferPtr>>& lora_a_weights,
            const std::vector<std::unordered_map<std::string, ft::ConstBufferPtr>>& lora_b_weights) = 0;

    virtual absl::Status removeLoRA(const int64_t lora_id)                    = 0;
    virtual absl::Status process(const std::list<GenerateStreamPtr>& streams) = 0;

    static GptModelDescription genModelDescription(const ft::GptInitParameter& params) {
        ft::RopeConfig rope_config;
        rope_config.embedding_style            = (ft::RopeType)params.rotary_embedding_style_;
        rope_config.embedding_dim              = params.rotary_embedding_dim_;
        rope_config.embedding_base             = params.rotary_embedding_base_;
        rope_config.rotary_embedding_scale     = params.rotary_embedding_scale_;
        rope_config.dynamic_embedding_max_pos  = params.dynamic_embedding_max_pos_;
        rope_config.org_embedding_max_pos      = params.org_embedding_max_pos_;
        rope_config.base_scale                 = params.base_scale_;
        rope_config.use_logn_attn              = params.use_logn_attn_;
        rope_config.logn_seq_len               = params.logn_seq_len_;

        ft::AttentionConfigs attention_config{
            params.head_num_ > 1 ? (size_t)params.head_num_ / params.tp_size_ : 1,
            params.head_num_kv_ > 1 ? (size_t)params.head_num_kv_ / params.tp_size_ : 1,
            (size_t)params.size_per_head_,
            rope_config,
            (size_t)params.seq_size_per_block_,
            params.is_causal_ ? fastertransformer::AttentionMaskType::causalMask :
                                fastertransformer::AttentionMaskType::noMask};
        auto moe_configs = params.moe_style_ ?
            (std::optional<ft::MoeConfigs>)ft::MoeConfigs({
                (size_t)params.expert_num_,
                (size_t)params.moe_k_,
                params.moe_normalize_expert_scale_,
                params.moe_inter_padding_size_ / params.tp_size_,
                params.has_moe_norm_
            }) : std::nullopt;
        ft::FfnConfigs ffn_config{
            ft::getActivationType(params.activation_type_str_),
            move(moe_configs),
        };
        return {attention_config,
                ffn_config,
                ft::getNormType(params.norm_type_str_),
                params.layernorm_eps_,
                (size_t)params.vocab_size_,
                params.layernorm_type_ == ft::LayerNormType::post_layernorm,
                params.input_embedding_scalar_};
    }

    virtual ~Executor(){};

public:
    ft::DeviceBase* device_;
};

}  // namespace rtp_llm
