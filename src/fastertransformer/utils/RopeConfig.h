#pragma once

#include <string>
#include "src/fastertransformer/utils/utils.h"

namespace fastertransformer {

enum class RopeStyle {
    No = 0,
    Base = 1,
    Glm2 = 2,
    DynamicNTK = 3,
    QwenDynamicNTK = 4,
    Yarn = 5,
    Llama3 = 6,
};

// low_freq_factor, high_freq_factor for llama3
// beta_slow, beta_fast for yarn

struct RopeConfig {
    RopeStyle style = RopeStyle::No;
    int dim = 0;
    int base = 10000;

    float scale = 1.0;
    float factor1 = 0;
    float factor2 = 0;
    int max_pos = 0;
    float extrapolation_factor = 1.0;
};

#define FT_ROPE_SWITCH(COND, CONST_NAME, ...)                           \
    [&] {                                                               \
        switch (COND) {                                                 \
            FT_SWITCH_ONE_CASE(CONST_NAME, RopeStyle::No, __VA_ARGS__)  \
            FT_SWITCH_ONE_CASE(CONST_NAME, RopeStyle::Base, __VA_ARGS__) \
            FT_SWITCH_ONE_CASE(CONST_NAME, RopeStyle::Glm2, __VA_ARGS__) \
            FT_SWITCH_ONE_CASE(CONST_NAME, RopeStyle::DynamicNTK,  __VA_ARGS__) \
            FT_SWITCH_ONE_CASE(CONST_NAME, RopeStyle::QwenDynamicNTK, __VA_ARGS__) \
            FT_SWITCH_ONE_CASE(CONST_NAME, RopeStyle::Yarn, __VA_ARGS__) \
            FT_SWITCH_ONE_CASE(CONST_NAME, RopeStyle::Llama3, __VA_ARGS__) \
        }                                                               \
    }()

} // namespace fastertransformer