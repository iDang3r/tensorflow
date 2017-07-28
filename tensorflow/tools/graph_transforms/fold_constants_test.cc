/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <utility>

#include "tensorflow/cc/ops/const_op.h"
#include "tensorflow/cc/ops/image_ops.h"
#include "tensorflow/cc/ops/nn_ops.h"
#include "tensorflow/cc/ops/sendrecv_ops.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/test_benchmark.h"
#include "tensorflow/core/public/session.h"
#include "tensorflow/tools/graph_transforms/fold_constants_lib.h"
#include "tensorflow/tools/graph_transforms/transform_utils.h"

namespace tensorflow {
namespace graph_transforms {

// Declaring this here so it doesn't need to be in the public header.
Status ReplaceSendRecvs(const GraphDef& original_graph_def,
                        const GraphDef& rewritten_graph_def,
                        const std::vector<string>& inputs,
                        const std::vector<string>& outputs,
                        GraphDef* output_graph_def);

class ConstantFoldingTest : public ::testing::Test {
 protected:
  void TestSimpleAdd() {
    auto root = tensorflow::Scope::NewRootScope();
    using namespace ::tensorflow::ops;  // NOLINT(build/namespaces)

    const int width = 100;

    Tensor a_data(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&a_data, 1.0f);
    Output a_const =
        Const(root.WithOpName("a_expect_removed"), Input::Initializer(a_data));

    Tensor b_data(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&b_data, 1.0f);
    Output b_const =
        Const(root.WithOpName("b_expect_removed"), Input::Initializer(b_data));

    Output add = Add(root.WithOpName("add_expect_removed"), a_const, b_const);

    Output placeholder =
        Placeholder(root.WithOpName("placeholder_expect_remains"), DT_FLOAT);

    Output mul =
        Mul(root.WithOpName("output_expect_remains"), add, placeholder);

    GraphDef graph_def;
    TF_ASSERT_OK(root.ToGraphDef(&graph_def));

    Tensor placeholder_tensor(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&placeholder_tensor, 1.0f);
    TestConstantFolding(graph_def,
                        {{"placeholder_expect_remains", placeholder_tensor}},
                        {}, {"output_expect_remains"});
  }

  void TestOpExclusionAdd() {
    auto root = tensorflow::Scope::NewRootScope();
    using namespace ::tensorflow::ops;  // NOLINT(build/namespaces)

    const int width = 100;

    Tensor a_data(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&a_data, 1.0f);
    Output a_const =
        Const(root.WithOpName("a_expect_remains"), Input::Initializer(a_data));

    Tensor b_data(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&b_data, 1.0f);
    Output b_const =
        Const(root.WithOpName("b_expect_remains"), Input::Initializer(b_data));

    Output add = Add(root.WithOpName("add_expect_remains"), a_const, b_const);

    Output placeholder =
        Placeholder(root.WithOpName("placeholder_expect_remains"), DT_FLOAT);

    Output mul =
        Mul(root.WithOpName("output_expect_remains"), add, placeholder);

    GraphDef graph_def;
    TF_ASSERT_OK(root.ToGraphDef(&graph_def));

    Tensor placeholder_tensor(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&placeholder_tensor, 1.0f);
    TestConstantFolding(graph_def,
                        {{"placeholder_expect_remains", placeholder_tensor}},
                        {"Add"}, {"output_expect_remains"});
  }

  void TestConstantFolding(const GraphDef& graph_def,
                           std::vector<std::pair<string, Tensor> > inputs,
                           std::vector<string> excluded_ops,
                           const std::vector<string>& outputs) {
    std::unique_ptr<tensorflow::Session> unfolded_session(
        tensorflow::NewSession(tensorflow::SessionOptions()));
    TF_ASSERT_OK(unfolded_session->Create(graph_def));
    std::vector<Tensor> unfolded_tensors;
    TF_ASSERT_OK(unfolded_session->Run(inputs, outputs, {}, &unfolded_tensors));

    GraphDef folded_graph_def;
    graph_transforms::TransformFuncContext context;
    for (const std::pair<string, Tensor>& input : inputs) {
      context.input_names.push_back(input.first);
    }
    context.output_names = outputs;
    context.params["exclude_op"] = std::move(excluded_ops);
    TF_ASSERT_OK(
        graph_transforms::FoldConstants(graph_def, context, &folded_graph_def));

    std::unique_ptr<tensorflow::Session> folded_session(
        tensorflow::NewSession(tensorflow::SessionOptions()));
    TF_ASSERT_OK(folded_session->Create(folded_graph_def));
    std::vector<Tensor> folded_tensors;
    TF_ASSERT_OK(folded_session->Run(inputs, outputs, {}, &folded_tensors));

    EXPECT_EQ(unfolded_tensors.size(), folded_tensors.size());
    for (int i = 0; i < unfolded_tensors.size(); ++i) {
      test::ExpectTensorNear<float>(unfolded_tensors[i], folded_tensors[i],
                                    1e-5);
    }

    std::map<string, const NodeDef*> folded_node_map;
    for (const NodeDef& node : folded_graph_def.node()) {
      folded_node_map.insert({node.name(), &node});
    }

    for (const NodeDef& node : graph_def.node()) {
      const StringPiece name(node.name());
      const int occurrence_count = folded_node_map.count(node.name());
      if (name.ends_with("expect_removed")) {
        EXPECT_EQ(0, occurrence_count) << "node.name()=" << node.name();
      }
      if (name.ends_with("expect_remains")) {
        EXPECT_EQ(1, occurrence_count) << "node.name()=" << node.name();
      }
    }
  }

  void TestReplaceSendRecvs() {
    using namespace ::tensorflow::ops;  // NOLINT(build/namespaces)

    const int width = 100;
    Tensor a_const_data(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&a_const_data, 1.0f);

    auto o_root = tensorflow::Scope::NewRootScope();
    _Recv(o_root.WithOpName("original_recv"), DT_FLOAT, "", "", 0, "");
    Output o_a_const =
        Const(o_root.WithOpName("a_const"), Input::Initializer(a_const_data));
    Placeholder(o_root.WithOpName("placeholder"), DT_FLOAT);
    _Send(o_root.WithOpName("original_send"), o_a_const, "", "", 0, "");
    GraphDef o_graph_def;
    TF_ASSERT_OK(o_root.ToGraphDef(&o_graph_def));

    auto n_root = tensorflow::Scope::NewRootScope();
    _Recv(n_root.WithOpName("original_recv"), DT_FLOAT, "", "", 0, "");
    Output n_a_const =
        Const(n_root.WithOpName("a_const"), Input::Initializer(a_const_data));
    _Recv(n_root.WithOpName("_recv_placeholder_0"), DT_FLOAT, "", "", 0, "");
    _Send(n_root.WithOpName("original_send"), n_a_const, "", "", 0, "");
    _Send(n_root.WithOpName("new_send"), n_a_const, "", "", 0, "");
    GraphDef n_graph_def;
    TF_ASSERT_OK(n_root.ToGraphDef(&n_graph_def));

    GraphDef result_graph_def;
    TF_ASSERT_OK(graph_transforms::ReplaceSendRecvs(
        o_graph_def, n_graph_def, {"placeholder"}, {"a_const"},
        &result_graph_def));

    std::map<string, const NodeDef*> node_map;
    graph_transforms::MapNamesToNodes(result_graph_def, &node_map);
    EXPECT_EQ(1, node_map.count("original_recv"));
    EXPECT_EQ(1, node_map.count("a_const"));
    EXPECT_EQ(1, node_map.count("placeholder"));
    EXPECT_EQ(1, node_map.count("original_send"));
    EXPECT_EQ(0, node_map.count("_recv_placeholder_0"));
    EXPECT_EQ(0, node_map.count("new_send"));
  }

  void TestRemoveUnusedNodes() {
    using namespace ::tensorflow::ops;  // NOLINT(build/namespaces)
    auto root = tensorflow::Scope::NewRootScope();
    using namespace ::tensorflow::ops;  // NOLINT(build/namespaces)

    const int width = 100;

    Tensor a_data(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&a_data, 1.0f);
    Output a_const = Const(root.WithOpName("a"), Input::Initializer(a_data));

    Tensor b_data(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&b_data, 1.0f);
    Output b_const = Const(root.WithOpName("b"), Input::Initializer(b_data));

    Output add = Add(root.WithOpName("add"), a_const, b_const);
    Output placeholder = Placeholder(root.WithOpName("placeholder"), DT_FLOAT);
    Output mul = Mul(root.WithOpName("output"), add, placeholder);

    Tensor unused_data(DT_FLOAT, TensorShape({width}));
    test::FillIota<float>(&unused_data, 1.0f);
    Output unused_const =
        Const(root.WithOpName("unused"), Input::Initializer(unused_data));

    GraphDef graph_def;
    TF_ASSERT_OK(root.ToGraphDef(&graph_def));
    GraphDef result_graph_def;
    TF_ASSERT_OK(graph_transforms::RemoveUnusedNodes(
        graph_def, {{"placeholder"}, {"output"}}, &result_graph_def));

    std::map<string, const NodeDef*> node_map;
    graph_transforms::MapNamesToNodes(result_graph_def, &node_map);
    EXPECT_EQ(1, node_map.count("a"));
    EXPECT_EQ(1, node_map.count("b"));
    EXPECT_EQ(1, node_map.count("add"));
    EXPECT_EQ(1, node_map.count("placeholder"));
    EXPECT_EQ(1, node_map.count("output"));
    EXPECT_EQ(0, node_map.count("unused"));
  }
};

TEST_F(ConstantFoldingTest, TestSimpleAdd) { TestSimpleAdd(); }

TEST_F(ConstantFoldingTest, TestOpExclusionAdd) { TestOpExclusionAdd(); }

TEST_F(ConstantFoldingTest, TestReplaceSendRecvs) { TestReplaceSendRecvs(); }

TEST_F(ConstantFoldingTest, TestRemoveUnusedNodes) { TestRemoveUnusedNodes(); }

}  // namespace graph_transforms
}  // namespace tensorflow
