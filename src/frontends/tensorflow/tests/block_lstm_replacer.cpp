// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "helper_transforms/block_lstm_replacer.hpp"

#include <gtest/gtest.h>

#include <frontend/shared/include/utils.hpp>
#include <openvino/frontend/manager.hpp>
#include <openvino/opsets/opset9.hpp>
#include <openvino/pass/manager.hpp>

#include "common_test_utils/ngraph_test_utils.hpp"
#include "gtest/gtest.h"
#include "helper_ops/block_lstm.hpp"

using namespace std;
using namespace ov;
using namespace opset9;
using namespace element;
using namespace frontend::tensorflow;
using namespace frontend::tensorflow::pass;

namespace {
shared_ptr<Model> gen_model(Dimension batch_size,
                            Dimension time_len,
                            int64_t hidden_size,
                            Dimension input_size,
                            float forget_bias,
                            float cell_clip,
                            bool use_peephole) {
    auto seq_len_max = make_shared<Parameter>(i64, Shape{});
    auto x = make_shared<Parameter>(f32, PartialShape{time_len, batch_size, input_size});
    auto cs_prev = make_shared<Parameter>(f32, PartialShape::dynamic());
    auto h_prev = make_shared<Parameter>(f32, PartialShape::dynamic());
    auto w = make_shared<Parameter>(f32, PartialShape{Dimension::dynamic(), 4 * hidden_size});
    auto wci = make_shared<Parameter>(f32, PartialShape::dynamic());
    auto wcf = make_shared<Parameter>(f32, PartialShape::dynamic());
    auto wco = make_shared<Parameter>(f32, PartialShape::dynamic());
    auto b = make_shared<Parameter>(f32, PartialShape::dynamic());

    auto block_lstm = make_shared<
        BlockLSTM>(seq_len_max, x, cs_prev, h_prev, w, wci, wcf, wco, b, forget_bias, cell_clip, use_peephole);

    return make_shared<Model>(OutputVector{block_lstm->output(6)},
                              ParameterVector{seq_len_max, x, cs_prev, h_prev, w, wci, wcf, wco, b});
}

shared_ptr<Model> gen_model_ref(Dimension m_batch_size,
                                Dimension m_time_len,
                                int64_t m_hidden_size,
                                Dimension m_input_size,
                                float forget_bias) {
    auto seq_len_max = make_shared<Parameter>(i64, Shape{});
    auto x = make_shared<Parameter>(f32, PartialShape{m_time_len, m_batch_size, m_input_size});
    auto cs_prev = make_shared<Parameter>(f32, PartialShape::dynamic());
    auto h_prev = make_shared<Parameter>(f32, PartialShape::dynamic());
    auto weights = make_shared<Parameter>(f32, PartialShape{Dimension::dynamic(), 4 * m_hidden_size});
    auto bias = make_shared<Parameter>(f32, PartialShape::dynamic());

    auto x_shape = make_shared<ShapeOf>(x, element::i64);
    auto ss_start = make_shared<Constant>(element::i64, Shape{1}, 2);
    auto ss_stop = make_shared<Constant>(element::i64, Shape{1}, 3);
    auto ss_step = make_shared<Constant>(element::i64, Shape{1}, 1);
    auto input_size = make_shared<StridedSlice>(x_shape,
                                                ss_start,
                                                ss_stop,
                                                ss_step,
                                                std::vector<int64_t>{0},
                                                std::vector<int64_t>{0});

    // retrieve the batch size
    // now x is in a format [time_len, batch_size, input_size]
    auto ss_start2 = make_shared<Constant>(element::i64, Shape{1}, 1);
    auto ss_stop2 = make_shared<Constant>(element::i64, Shape{1}, 2);
    auto batch_size = make_shared<StridedSlice>(x_shape,
                                                ss_start2,
                                                ss_stop2,
                                                ss_step,
                                                std::vector<int64_t>{0},
                                                std::vector<int64_t>{0});
    auto hidden_size_const = make_shared<Constant>(element::i64, Shape{1}, std::vector<int64_t>{m_hidden_size});

    // adjust weights and bias
    // 1. reshape weights and bias to highlight channel dimension
    auto new_weight_shape = make_shared<Constant>(element::i64, Shape{3}, std::vector<int64_t>{0, 4, -1});
    auto weight_reshape = make_shared<Reshape>(weights, new_weight_shape, true);
    auto new_bias_shape = make_shared<Constant>(element::i64, Shape{2}, std::vector<int64_t>{4, -1});
    auto bias_reshape = make_shared<Reshape>(bias, new_bias_shape, true);
    // 2. reorder gates icfo --> fico for both weights and biases
    auto reorder_const = make_shared<Constant>(element::i64, Shape{4}, std::vector<int64_t>{2, 0, 1, 3});
    auto weights_axis = make_shared<Constant>(element::i64, Shape{}, 1);
    auto weights_reorder = make_shared<Gather>(weight_reshape, reorder_const, weights_axis);
    auto bias_axis = make_shared<Constant>(element::i64, Shape{}, 0);
    auto bias_reorder = make_shared<Gather>(bias_reshape, reorder_const, bias_axis);
    // 3. shift_const.value should be added to the first 1 / 4th part of the biases(f - gate : 0)
    auto shift_const = make_shared<Constant>(element::f32, Shape{}, forget_bias);
    auto bias_split_lens = make_shared<Constant>(element::i64, Shape{2}, std::vector<int64_t>{1, 3});
    auto bias_split = make_shared<VariadicSplit>(bias_reorder, bias_axis, bias_split_lens);
    auto bias_first_shift = make_shared<Add>(bias_split->output(0), shift_const);
    auto bias_shift = make_shared<Concat>(OutputVector{bias_first_shift, bias_split->output(1)}, 0);
    // 4. return to the original shapes
    auto new_weight_shape2 = make_shared<Constant>(element::i64, Shape{2}, std::vector<int64_t>{0, -1});
    auto weight_reshape2 = make_shared<Reshape>(weights_reorder, new_weight_shape2, true);
    // 5. normalize weights and bias
    auto transpose_order = make_shared<Constant>(element::i64, Shape{2}, std::vector<int64_t>{1, 0});
    auto new_bias_shape2 = make_shared<Constant>(element::i64, Shape{1}, std::vector<int64_t>{-1});
    auto weights_normalize = make_shared<Transpose>(weight_reshape2, transpose_order);
    auto bias_normalized = make_shared<Reshape>(bias_shift, new_bias_shape2, true);
    // 6. split weights into W and R inputs
    auto WR_split_axis = make_shared<Constant>(element::i64, Shape{}, 1);
    auto WR_split_lens = make_shared<Concat>(OutputVector{input_size, hidden_size_const}, 0);
    auto WR_split = make_shared<VariadicSplit>(weights_normalize, WR_split_axis, WR_split_lens);
    // 7. unsqueeze weights and bias to have a dimension for a number of directions
    auto num_direct_axis = make_shared<Constant>(element::i64, Shape{1}, std::vector<int64_t>{0});
    auto W = make_shared<Unsqueeze>(WR_split->output(0), num_direct_axis);
    auto R = make_shared<Unsqueeze>(WR_split->output(1), num_direct_axis);
    auto B = make_shared<Unsqueeze>(bias_normalized, num_direct_axis);

    // normalize initial hidden and cell states
    auto unsqueeze_axis = make_shared<Constant>(element::i64, Shape{1}, std::vector<int64_t>{1});
    auto init_hidden_state = make_shared<Unsqueeze>(h_prev, unsqueeze_axis);
    auto init_cell_state = make_shared<Unsqueeze>(cs_prev, unsqueeze_axis);

    // prepare sequence length input for LSTMSequence
    auto seq_len_max_adjusted = make_shared<Broadcast>(seq_len_max, batch_size);

    // prepare input data since LSTMSequence accept it in a format [batch_size, time_len, input_size]
    auto x_order = make_shared<Constant>(element::i64, Shape{3}, std::vector<int64_t>{1, 0, 2});
    auto x_adjusted = make_shared<Transpose>(x, x_order);

    // create LSTMSequence node and reconnect inputs and normalized weights and bias
    auto lstm_sequence = make_shared<LSTMSequence>(x_adjusted,
                                                   init_hidden_state,
                                                   init_cell_state,
                                                   seq_len_max_adjusted,
                                                   W,
                                                   R,
                                                   B,
                                                   m_hidden_size,
                                                   LSTMSequence::direction::FORWARD);

    // adjust output of concatenated of hidden states from LSTMSequence to have it in a format [time_len,
    // batch_size, hidden_size]
    // 1. squeeze extra dimension - num_directions
    auto squeeze_axis = make_shared<Constant>(element::i64, Shape{1}, std::vector<int64_t>{1});
    auto squeeze_output_hidden_states = make_shared<Squeeze>(lstm_sequence->output(0), squeeze_axis);
    // 2. transpose the output to rotate batch and time dimensions
    auto output_hidden_states_order = make_shared<Constant>(element::i64, Shape{3}, std::vector<int64_t>{1, 0, 2});
    auto output_hidden_states = make_shared<Transpose>(squeeze_output_hidden_states, output_hidden_states_order);

    return make_shared<Model>(OutputVector{output_hidden_states->output(0)},
                              ParameterVector{seq_len_max, x, cs_prev, h_prev, weights, bias});
}

}  // namespace

TEST_F(TransformationTestsF, BlockLSTMReplacerOneOutput) {
    {
        function = gen_model(2, 10, 120, 20, 1.0f, -1.0f, false);
        manager.register_pass<BlockLSTMToLSTMSequenceOneOutput>();
    }
    { function_ref = gen_model_ref(2, 10, 120, 20, 1.0f); }
}

TEST_F(TransformationTestsF, BlockLSTMReplacerOneOutputPeepHole) {
    {
        function = gen_model(2, 10, 120, 20, 1.0f, -1.0f, true);
        manager.register_pass<BlockLSTMToLSTMSequenceOneOutput>();
    }
    {
        // the transformation is not applied for the peep hole case
        function_ref = gen_model(2, 10, 120, 20, 1.0f, -1.0f, true);
    }
}
