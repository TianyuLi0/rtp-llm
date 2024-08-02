
#include "gtest/gtest.h"

#include "maga_transformer/cpp/lora/LoraManager.h"
#include "src/fastertransformer/devices/testing/TestBase.h"

#include <chrono>
#include <memory>
#include <thread>
#include <array>

using namespace std;

namespace rtp_llm {

class LoraManagerTest: public DeviceTestBase {
protected:

std::array<ft::lora::loraLayerWeightsMap, 2> mockLoraLayerWeightMap(int layer_num, int m, int n, int rank,
                                                                    std::vector<std::string> target_modules)
{
    ft::lora::loraLayerWeightsMap lora_a_map(layer_num);
    ft::lora::loraLayerWeightsMap lora_b_map(layer_num);
    for (int i = 0; i < layer_num; i ++) {
        for (auto target_module : target_modules) {
            BufferPtr lora_a_buffer_ptr = tensorToBuffer(torch::rand({m, rank}));
            BufferPtr lora_b_buffer_ptr = tensorToBuffer(torch::rand({rank, n}));
            lora_a_map[i][target_module] = lora_a_buffer_ptr;
            lora_b_map[i][target_module] = lora_b_buffer_ptr;
        }
    }
    return std::array<ft::lora::loraLayerWeightsMap, 2>({lora_a_map, lora_b_map});
}


};

TEST_F(LoraManagerTest, testSimple) {
    auto lora_manager = lora::LoraManager();
    ft::lora::loraLayerWeightsMap lora_a_map(1);
    ft::lora::loraLayerWeightsMap lora_b_map(1);
    EXPECT_EQ(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), false);

    lora_manager.addLora(0, lora_a_map, lora_b_map);
    EXPECT_NE(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), true);

    lora_manager.removeLora(0);
    EXPECT_EQ(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), false);
}

TEST_F(LoraManagerTest, testPressure) {
    auto lora_manager = lora::LoraManager();
    ft::lora::loraLayerWeightsMap lora_a_map(1);
    ft::lora::loraLayerWeightsMap lora_b_map(1);
    EXPECT_EQ(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), false);

    for (int i = 0; i < 1000; i++) {
        lora_manager.addLora(i, lora_a_map, lora_b_map);
        EXPECT_NE(lora_manager.getLora(i), nullptr);
        EXPECT_EQ(lora_manager.hasLora(i), true);
    }

    for (int i = 0; i < 1000; i++) {
        lora_manager.removeLora(i);
        EXPECT_EQ(lora_manager.getLora(i), nullptr);
        EXPECT_EQ(lora_manager.hasLora(i), false);
    }
}

TEST_F(LoraManagerTest, testRemoveSimple) {
    auto lora_manager = lora::LoraManager();
    ft::lora::loraLayerWeightsMap lora_a_map(1);
    ft::lora::loraLayerWeightsMap lora_b_map(1);
    EXPECT_EQ(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), false);

    lora_manager.addLora(0, lora_a_map, lora_b_map);
    EXPECT_EQ(lora_manager.hasLora(0), true);
    {
        auto lora_resource = lora_manager.getLora(0);

        EXPECT_NE(lora_resource, nullptr);
        EXPECT_EQ(lora_resource.use_count(), 2);
    }
    auto lora_resource = lora_manager.getLora(0);

    EXPECT_NE(lora_resource, nullptr);
    EXPECT_EQ(lora_resource.use_count(), 2);
    auto removeLoraFunc = [&lora_manager](int64_t lora_id) {
        lora_manager.removeLora(lora_id);
    };
    std::thread removeLora(removeLoraFunc, 0);
    std::this_thread::sleep_for(2000ms);
    EXPECT_NE(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), true);
    lora_resource = nullptr;
    lora_manager.releaseSignal();
    removeLora.join();
    EXPECT_EQ(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), false);
}

TEST_F(LoraManagerTest, testisLoraAliveSimple) {
    auto lora_manager = lora::LoraManager();
    ft::lora::loraLayerWeightsMap lora_a_map(1);
    ft::lora::loraLayerWeightsMap lora_b_map(1);
    EXPECT_EQ(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), false);

    lora_manager.addLora(0, lora_a_map, lora_b_map);
    EXPECT_EQ(lora_manager.hasLora(0), true);
    auto lora_resource = lora_manager.getLora(0);

    EXPECT_NE(lora_resource, nullptr);
    EXPECT_EQ(lora_resource.use_count(), 2);

    // start remove lora
    auto removeLoraFunc = [&lora_manager](int64_t lora_id) {
        lora_manager.removeLora(lora_id);
    };
    std::thread removeLora(removeLoraFunc, 0);
    // when lora resource ref cout not release and removing lora
    // we can still get lora resource from getLora.
    std::this_thread::sleep_for(2000ms);
    EXPECT_EQ(lora_manager.hasLora(0), true);
    EXPECT_NE(lora_manager.getLora(0), nullptr);


    // after lora resource release, we can not get lora.
    // hasLora will return nullptr.
    lora_resource = nullptr;
    lora_manager.releaseSignal();
    removeLora.join();
    EXPECT_EQ(lora_manager.getLora(0), nullptr);
    EXPECT_EQ(lora_manager.hasLora(0), false);
}


TEST_F(LoraManagerTest, testMultiAddWithMultiRemove) {
    auto lora_manager = lora::LoraManager();

    auto addLoraFunc = [&](size_t lora_num) {
        ft::lora::loraLayerWeightsMap lora_a_map(1);
        ft::lora::loraLayerWeightsMap lora_b_map(1);
        for (int i = 0; i < lora_num; i ++) {
            lora_manager.addLora(i, lora_a_map, lora_b_map);
        }
    };

    auto removeLoraFunc = [&](size_t lora_num) {
        for (int i = 0; i < lora_num; i ++) {
            while(true) {
                if (lora_manager.hasLora(i)) {
                    break;
                }
            }
            lora_manager.removeLora(i);
        }
    };

    std::thread addLora(addLoraFunc, 1000);
    std::thread removeLora(removeLoraFunc, 1000);

    addLora.join();
    removeLora.join();

    for (int i = 0; i < 1000; i++) {
        EXPECT_EQ(lora_manager.hasLora(0), false);
        EXPECT_EQ(lora_manager.getLora(0), nullptr);
    }
}


TEST_F(LoraManagerTest, testMakeLoraModelInput) {
    auto lora_manager = lora::LoraManager();
    int layer_num = 4;
    auto lora_map_1 = mockLoraLayerWeightMap(layer_num, 64, 64, 8, {ft::W::attn_qkv_w});
    auto lora_map_2 = mockLoraLayerWeightMap(layer_num, 64, 64, 8, {ft::W::ffn_w1});
    auto lora_map_3 = mockLoraLayerWeightMap(layer_num, 64, 64, 8, {ft::W::ffn_w2});
    lora_manager.addLora(0, lora_map_1[0], lora_map_1[1]);
    lora_manager.addLora(1, lora_map_2[0], lora_map_2[1]);
    lora_manager.addLora(2, lora_map_3[0], lora_map_3[1]);

    auto lora_ids = createBuffer<int32_t>({8}, {-1, 0, 0, 1, 2, -1, -1, -1}, ft::AllocationType::HOST);
    auto lora_input_lengths = createBuffer<int32_t>({8}, {1, 1, 1, 1, 1, 1, 1, 1}, ft::AllocationType::HOST);
    auto lora_model_input_ptr = lora_manager.makeLoraModelInput(lora_ids, lora_input_lengths);

    EXPECT_EQ(lora_model_input_ptr->lora_model_input_[0], nullptr);
    EXPECT_EQ(lora_model_input_ptr->lora_model_input_[5], nullptr);
    EXPECT_EQ(lora_model_input_ptr->lora_model_input_[6], nullptr);
    EXPECT_EQ(lora_model_input_ptr->lora_model_input_[7], nullptr);

    for (int i = 0; i < layer_num; i++) {
        auto attn_qkv_w_lora_input = lora_model_input_ptr->getOpInput(i, ft::W::attn_qkv_w);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_a_[0], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_b_[0], nullptr);

        EXPECT_NE(attn_qkv_w_lora_input.lora_a_[1], nullptr);
        EXPECT_NE(attn_qkv_w_lora_input.lora_b_[1], nullptr);
        auto lora_a_1_tensor = bufferToTensor(*std::const_pointer_cast<Buffer>(attn_qkv_w_lora_input.lora_a_[1]));
        auto lora_b_1_tensor = bufferToTensor(*std::const_pointer_cast<Buffer>(attn_qkv_w_lora_input.lora_b_[1]));
        auto lora_a_1_ref = bufferToTensor(*std::const_pointer_cast<Buffer>(lora_map_1[0][i][ft::W::attn_qkv_w]));
        auto lora_b_1_ref = bufferToTensor(*std::const_pointer_cast<Buffer>(lora_map_1[1][i][ft::W::attn_qkv_w]));
        torch::equal(lora_a_1_tensor, lora_a_1_ref);
        torch::equal(lora_b_1_tensor, lora_b_1_ref);
        EXPECT_NE(attn_qkv_w_lora_input.lora_a_[2], nullptr);
        EXPECT_NE(attn_qkv_w_lora_input.lora_b_[2], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_a_[3], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_b_[3], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_a_[4], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_b_[4], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_a_[5], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_b_[5], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_a_[6], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_b_[6], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_a_[7], nullptr);
        EXPECT_EQ(attn_qkv_w_lora_input.lora_b_[7], nullptr);
    }

    for (int i = 0; i < layer_num; i++) {
        auto ffn_w1_lora_input = lora_model_input_ptr->getOpInput(i, ft::W::ffn_w1);
        EXPECT_EQ(ffn_w1_lora_input.lora_a_[0], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_b_[0], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_a_[1], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_b_[1], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_a_[2], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_b_[2], nullptr);
        EXPECT_NE(ffn_w1_lora_input.lora_a_[3], nullptr);
        EXPECT_NE(ffn_w1_lora_input.lora_b_[3], nullptr);
        auto lora_a_1_tensor = bufferToTensor(*std::const_pointer_cast<Buffer>(ffn_w1_lora_input.lora_a_[3]));
        auto lora_b_1_tensor = bufferToTensor(*std::const_pointer_cast<Buffer>(ffn_w1_lora_input.lora_b_[3]));
        auto lora_a_1_ref = bufferToTensor(*std::const_pointer_cast<Buffer>(lora_map_2[0][i][ft::W::ffn_w1]));
        auto lora_b_1_ref = bufferToTensor(*std::const_pointer_cast<Buffer>(lora_map_2[1][i][ft::W::ffn_w1]));
        torch::equal(lora_a_1_tensor, lora_a_1_ref);
        torch::equal(lora_b_1_tensor, lora_b_1_ref);
        EXPECT_EQ(ffn_w1_lora_input.lora_a_[4], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_b_[4], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_a_[5], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_b_[5], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_a_[6], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_b_[6], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_a_[7], nullptr);
        EXPECT_EQ(ffn_w1_lora_input.lora_b_[7], nullptr);
    }

    for (int i = 0; i < layer_num; i++) {
        auto ffn_w2_lora_input = lora_model_input_ptr->getOpInput(i, ft::W::ffn_w2);
        EXPECT_EQ(ffn_w2_lora_input.lora_a_[0], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_b_[0], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_a_[1], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_b_[1], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_a_[2], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_b_[2], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_a_[3], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_b_[3], nullptr);
        EXPECT_NE(ffn_w2_lora_input.lora_a_[4], nullptr);
        EXPECT_NE(ffn_w2_lora_input.lora_b_[4], nullptr);
        auto lora_a_1_tensor = bufferToTensor(*std::const_pointer_cast<Buffer>(ffn_w2_lora_input.lora_a_[4]));
        auto lora_b_1_tensor = bufferToTensor(*std::const_pointer_cast<Buffer>(ffn_w2_lora_input.lora_b_[4]));
        auto lora_a_1_ref = bufferToTensor(*std::const_pointer_cast<Buffer>(lora_map_3[0][i][ft::W::ffn_w2]));
        auto lora_b_1_ref = bufferToTensor(*std::const_pointer_cast<Buffer>(lora_map_3[1][i][ft::W::ffn_w2]));
        torch::equal(lora_a_1_tensor, lora_a_1_ref);
        torch::equal(lora_b_1_tensor, lora_b_1_ref);
        EXPECT_EQ(ffn_w2_lora_input.lora_a_[5], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_b_[5], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_a_[6], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_b_[6], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_a_[7], nullptr);
        EXPECT_EQ(ffn_w2_lora_input.lora_b_[7], nullptr);
    }
}

}  // namespace rtp_llm
