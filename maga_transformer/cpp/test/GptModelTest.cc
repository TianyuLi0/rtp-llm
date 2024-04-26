#include "src/fastertransformer/devices/testing/TestBase.h"

#define private public

#ifdef GOOGLE_CUDA
#include "src/fastertransformer/devices/cuda_impl/CudaDevice.h"
#endif
#include "maga_transformer/cpp/models/GptModel.h"
#include "maga_transformer/cpp/test/ModelTestUtil.h"
#include "src/fastertransformer/devices/utils/DebugUtils.h"
#include "src/fastertransformer/devices/torch_impl/GptModel.hpp"

using namespace std;
using namespace rtp_llm;
using namespace fastertransformer;

class GptModelTest: public DeviceTestBase {
};

TEST_F(GptModelTest, testSimple) {
    const auto path = test_data_path_ + "../../test/model_test/fake_test/testdata/qwen_0.5b";
    auto weights = loadWeightsFromDir(path);
    assert(weights->lm_head->kernel);
    assert(weights->embedding);
    assert(weights->layers.size() == 24);

    GptModelDescription description;
    description.activation_type = ActivationType::Swiglu;
    description.norm_type = NormType::rmsnorm;
    auto& attention_conf = description.attention_conf;
    attention_conf.head_num = 16;
    attention_conf.kv_head_num = 16;
    attention_conf.size_per_head = 64;
    attention_conf.hidden_size = 1024;
    attention_conf.tokens_per_block = 8;
    attention_conf.rope_config.embedding_style = RopeType::Base;
    attention_conf.rope_config.embedding_dim = 64;
    attention_conf.rope_config.embedding_base = 1000000;
    GptModel model({device_, *weights, description});

    const auto cache_block_num = 128;
    CacheConfig cache_config(
        weights->layers.size(),
        cache_block_num,
        attention_conf.kv_head_num,
        attention_conf.size_per_head,
        attention_conf.tokens_per_block,
        DataType::TYPE_FP16
    );

    const std::vector<int32_t> input_lengths_vec = {3};
    const std::vector<int32_t> sequence_lengths_vec = {};

    auto combo_tokens = createBuffer<int32_t>({3}, {13048, 11, 220}, AllocationType::HOST);
    auto input_lengths = createBuffer<int32_t>({1}, input_lengths_vec, AllocationType::HOST);
    auto sequence_lengths = createBuffer<int32_t>({0}, sequence_lengths_vec, AllocationType::HOST);
    auto kv_cache_blocks = allocateKVBlocks(cache_config, input_lengths_vec, sequence_lengths_vec);
    const auto mask_tensor = create_context_mask(input_lengths_vec).to(torch::kFloat16);
    const auto mask_buf = tensorToBuffer(mask_tensor);

    GptModelInputs inputs = {
        std::move(combo_tokens), std::move(input_lengths), std::move(sequence_lengths)
    };
    inputs.attention_mask = *mask_buf;
    inputs.kv_cache_blocks = std::move(kv_cache_blocks);

    try {
        auto outputs = model.forward(inputs);
        printBufferData(*outputs.logits, "logits");
    } catch (const std::exception& e) {
        if (DeviceFactory::getDefaultDevice()->type() == "cuda") {
            throw e;
        } else {
            // temporarily we allow CPU test to fail
            std::cout << "exception: " << e.what() << std::endl;
        }
    }
}

TEST_F(GptModelTest, testAttentionInputs) {
    GptModelDescription description;
    Weights weights;
    GptModel model({device_, weights, description});
    GptModelInputs inputs;
    inputs.kv_cache_blocks = createBuffer<int64_t>({1, 2, 1, 10}, {0}, AllocationType::HOST);
    inputs.input_lengths = createBuffer<int32_t>({4}, {3, 5, 2, 7}, AllocationType::HOST);
    inputs.sequence_lengths = createBuffer<int32_t>({0}, {}, AllocationType::HOST);
    inputs.combo_tokens = createBuffer<int32_t>({17}, std::vector<int32_t>(17, 0), AllocationType::HOST);

    {
        auto attention_inputs = model.prepareAttentionInputs(inputs);
        printBuffer<int32_t>(*attention_inputs.cu_seqlens);
        printBuffer<int32_t>(*attention_inputs.padding_offset);
        assertBufferValueEqual<int32_t>(*attention_inputs.cu_seqlens, {0, 3, 8, 10, 17});
        assertBufferValueEqual<int32_t>(*attention_inputs.padding_offset,
            {0, 0, 0, 4, 4, 4, 4, 4, 6, 6, 11, 11, 11, 11, 11, 11, 11});
        ASSERT_EQ(attention_inputs.context_batch_size, 4);
        ASSERT_EQ(attention_inputs.context_max_seq_len, 7);
        ASSERT_EQ(attention_inputs.decoder_batch_size, 0);
        ASSERT_EQ(attention_inputs.decoder_max_seq_len, 0);
    }

    inputs.sequence_lengths = createBuffer<int32_t>({3}, {4, 19, 23}, AllocationType::HOST);
    inputs.combo_tokens = createBuffer<int32_t>({7}, std::vector<int32_t>(7, 0), AllocationType::HOST);
    {
        auto attention_inputs = model.prepareAttentionInputs(inputs);
        printBuffer<int32_t>(*attention_inputs.cu_seqlens);
        printBuffer<int32_t>(*attention_inputs.padding_offset);
        assertBufferValueEqual<int32_t>(*attention_inputs.cu_seqlens, {0, 7});
        assertBufferValueEqual<int32_t>(*attention_inputs.padding_offset,
            {0, 0, 0, 0, 0, 0, 0});
        ASSERT_EQ(attention_inputs.context_batch_size, 1);
        ASSERT_EQ(attention_inputs.context_max_seq_len, 7);
        ASSERT_EQ(attention_inputs.decoder_batch_size, 3);
        ASSERT_EQ(attention_inputs.decoder_max_seq_len, 23);
    }

    inputs.sequence_lengths = createBuffer<int32_t>({2}, {4, 6}, AllocationType::HOST);
    inputs.combo_tokens = createBuffer<int32_t>({9}, std::vector<int32_t>(9, 0), AllocationType::HOST);
    {
        auto attention_inputs = model.prepareAttentionInputs(inputs);
        printBuffer<int32_t>(*attention_inputs.cu_seqlens);
        printBuffer<int32_t>(*attention_inputs.padding_offset);
        assertBufferValueEqual<int32_t>(*attention_inputs.cu_seqlens, {0, 2, 9});
        assertBufferValueEqual<int32_t>(*attention_inputs.padding_offset,
            {0, 0, 5, 5, 5, 5, 5, 5, 5});
        ASSERT_EQ(attention_inputs.context_batch_size, 2);
        ASSERT_EQ(attention_inputs.context_max_seq_len, 7);
        ASSERT_EQ(attention_inputs.decoder_batch_size, 2);
        ASSERT_EQ(attention_inputs.decoder_max_seq_len, 6);
    }
}
