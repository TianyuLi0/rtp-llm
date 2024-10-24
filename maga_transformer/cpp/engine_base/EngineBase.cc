#include "maga_transformer/cpp/engine_base/EngineBase.h"
#include "maga_transformer/cpp/utils/SignalUtils.h"
#include "src/fastertransformer/devices/DeviceFactory.h"
#include "autil/EnvUtil.h"
#include <stdexcept>

using namespace autil;

namespace rtp_llm {

EngineBase::EngineBase(const EngineInitParams& params) {
    EngineBase::initDevices(params);
    device_       = ft::DeviceFactory::getDefaultDevice();
    lora_manager_ = std::make_shared<lora::LoraManager>();
}

void EngineBase::initDevices(const EngineInitParams& params) {
    fastertransformer::Logger::getEngineLogger().setRank(params.gpt_init_parameter.tp_rank_);

    FT_LOG_INFO("install sighandler begin");
    fastertransformer::Logger::getEngineLogger().flush();    
    if (!installSighandler()) {
        FT_LOG_ERROR("install sighandler failed");
        std::runtime_error("install sighandler failed");
    }
    FT_LOG_INFO("install sighandler success");
    auto  global_params               = ft::DeviceFactory::getDefaultGlobalDeviceParams();
    auto& default_device_params       = global_params.device_params[0].second;
    default_device_params.tp_size     = params.gpt_init_parameter.tp_size_;
    default_device_params.tp_rank     = params.gpt_init_parameter.tp_rank_;
    default_device_params.device_id   = params.gpt_init_parameter.local_rank_;
    default_device_params.master_ip   = params.gpt_init_parameter.nccl_ip_;
    default_device_params.master_port = params.gpt_init_parameter.nccl_port_;
    default_device_params.tokens_per_block = params.gpt_init_parameter.seq_size_per_block_;
    int max_batch_size =
        params.gpt_init_parameter.max_context_batch_size_ + params.gpt_init_parameter.max_generate_batch_size_;
    default_device_params.max_batch_size =
        std::max(1024, max_batch_size * 2);  // set static max batch size to avoid sampler reset memory
    default_device_params.device_reserve_memory_bytes =
        -256L * 1024 * 1024 * std::min(4, (int)default_device_params.tp_size);  // 256MB, and need more when tp > 1
    if (params.gpt_init_parameter.is_multimodal_) {
        default_device_params.device_reserve_memory_bytes -= 4L * 1024 * 1024 * 1024;
    }
    default_device_params.host_reserve_memory_bytes = 4L * 1024 * 1024 * 1024;  // 4GB
    int64_t device_reserve_memory_bytes             = EnvUtil::getEnv("DEVICE_RESERVE_MEMORY_BYTES", (int64_t)0);
    int64_t host_reserve_memory_bytes               = EnvUtil::getEnv("HOST_RESERVE_MEMORY_BYTES", (int64_t)0);
    default_device_params.device_reserve_memory_bytes =
        device_reserve_memory_bytes ? device_reserve_memory_bytes : default_device_params.device_reserve_memory_bytes;
    default_device_params.host_reserve_memory_bytes =
        host_reserve_memory_bytes ? host_reserve_memory_bytes : default_device_params.host_reserve_memory_bytes;
    ft::DeviceFactory::initDevices(global_params);
}

void EngineBase::addLora(const std::string&            adapter_name,
                         ft::lora::loraLayerWeightsMap lora_a,
                         ft::lora::loraLayerWeightsMap lora_b) {
    lora_manager_->addLora(adapter_name, lora_a, lora_b);
}

void EngineBase::removeLora(const std::string& adapter_name) {
    lora_manager_->removeLora(adapter_name);
}

std::shared_ptr<lora::LoraManager> EngineBase::getLoraManager() {
    return lora_manager_;
}

}  // namespace rtp_llm
