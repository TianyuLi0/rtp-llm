#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include "torch/all.h"
#include "absl/status/status.h"
#include "maga_transformer/cpp/engine_base/Engine.h"
#include "maga_transformer/cpp/batch_stream_processor/BatchStreamProcessor.h"
#include "maga_transformer/cpp/cache/CacheManager.h"
#include "maga_transformer/cpp/dataclass/MagaInitParameter.h"
#include "maga_transformer/cpp/engine_base/Executor.h"
#include "maga_transformer/cpp/schedulers/SchedulerBase.h"
#include "maga_transformer/cpp/common/fatal_util.h"

namespace rtp_llm {

class SpeculativeEngine: public Engine {
public:
    SpeculativeEngine(const MagaInitParams&                                                   params,
                      const std::vector<std::unordered_map<std::string, ft::ConstBufferPtr>>& layer_weights,
                      const std::unordered_map<std::string, ft::ConstBufferPtr>&              weights);
    ~SpeculativeEngine();
    absl::Status step();
    absl::Status stop() override;
    absl::Status startLoop();
    absl::Status enqueue(std::shared_ptr<GenerateStream>& stream) override;
    void addLoRA(const int64_t                                                   lora_id,
                 const std::vector<std::unordered_map<std::string, ft::ConstBufferPtr>>& lora_a_weights,
                 const std::vector<std::unordered_map<std::string, ft::ConstBufferPtr>>& lora_b_weights) override {
        RAISE_FATAL_ERROR("Speculative not support lora now");
    }
    void removeLoRA(const int64_t lora_id) override {
        RAISE_FATAL_ERROR("Speculative not support lora now");
    }

private:
    absl::StatusOr<std::list<GenerateStreamPtr>>
         generateDraftStreams(const std::list<GenerateStreamPtr>& target_streams);
    void loop();

private:
    std::thread                           loop_thread_;
    std::atomic<bool>                     running_;
    std::unique_ptr<Executor>             draft_executor_;
    std::unique_ptr<Executor>             target_executor_;
    std::unique_ptr<BatchStreamProcessor> batch_stream_processor_;
    std::unique_ptr<SchedulerBase>        scheduler_;
    std::shared_ptr<CacheManager>         draft_cache_manager_;
    std::shared_ptr<CacheManager>         target_cache_manager_;
    GptInitParameter                      params_;
};

}  // namespace rtp_llm