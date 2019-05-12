/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/tf2xla/rearrange_function_argument_pass.h"

#include <algorithm>

#include "tensorflow/compiler/tf2xla/tf2xla_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/process_function_library_runtime.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/graph_to_functiondef.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"
#include "tensorflow/core/public/session_options.h"
#include "tensorflow/core/public/version.h"
#include "tensorflow/core/util/dump_graph.h"

namespace tensorflow {

namespace {

// Given original input types and argument index mapping, return the new input
// types.
std::vector<DataType> ShuffleInputDataTypeAttribute(
    const std::vector<DataType>& in_types,
    const std::vector<int>& index_mapping) {
  std::vector<DataType> result(index_mapping.size());
  for (int i = 0; i < in_types.size(); i++) {
    result[index_mapping.at(i)] = in_types[i];
  }
  return result;
}

// Given original input types, check if we need to rewrite the function (by
// checking if all DT_RESOURCE inputs are in the end). If the function needs to
// be rewritten, `resource_input_count` will be set to number of DT_RESOURCE
// inputs, and `index_mapping` will hold a mapping for original input index to
// rearranged input index.
Status InputTypesNeedsRearrange(const std::vector<DataType>& in_types,
                                bool* need_rewrite, int* resource_input_count,
                                std::vector<int>* index_mapping) {
  int first_resource_index = -1;
  for (int i = 0; i < in_types.size(); i++) {
    DataType type = in_types[i];
    if (type == DT_RESOURCE) {
      first_resource_index = i;
      break;
    }
  }
  if (first_resource_index == -1) {
    // No resource input. No need to rewrite.
    *need_rewrite = false;
    return Status::OK();
  }

  *need_rewrite = false;
  for (int i = first_resource_index + 1; i < in_types.size(); i++) {
    if (in_types[i] != DT_RESOURCE) {
      *need_rewrite = true;
      break;
    }
  }
  if (!*need_rewrite) {
    return Status::OK();
  }

  *resource_input_count = 0;
  for (int i = 0; i < in_types.size(); i++) {
    DataType type = in_types[i];
    if (type == DT_RESOURCE) {
      ++(*resource_input_count);
    }
  }
  int non_resource_index = 0,
      resource_index = in_types.size() - *resource_input_count;
  index_mapping->resize(in_types.size());
  for (int i = 0; i < in_types.size(); i++) {
    if (in_types[i] != DT_RESOURCE) {
      (*index_mapping)[i] = non_resource_index;
      non_resource_index++;
    } else {
      (*index_mapping)[i] = resource_index;
      resource_index++;
    }
  }

  return Status::OK();
}

// Given mapping between original input index and rearranged input index,
// reorder input edges for the node.
Status ReorderInputEdges(Graph* g, Node* n,
                         const std::vector<int>& index_mapping) {
  std::vector<const Edge*> input_edges;
  for (const Edge* e : n->in_edges()) {
    if (e->IsControlEdge()) {
      continue;
    }
    input_edges.push_back(e);
  }
  for (const Edge* e : input_edges) {
    Node* src = e->src();
    int src_output = e->src_output();
    int dst_input = e->dst_input();
    int new_dst_input = index_mapping.at(dst_input);
    g->RemoveEdge(e);
    g->AddEdge(src, src_output, n, new_dst_input)->DebugString();
  }
  return Status::OK();
}

// For While node, given mapping between original input index and rearranged
// input index, reorder output edges for the node. DT_RESOURCE outputs are
// removed from the node and we will use the node's corresponding input for the
// edge.
Status ReorderOutputEdges(Graph* g, Node* n, int input_count,
                          int resource_input_count,
                          const std::vector<int>& index_mapping) {
  std::vector<const Edge*> output_edges;
  for (const Edge* e : n->out_edges()) {
    if (e->IsControlEdge()) {
      continue;
    }
    output_edges.push_back(e);
  }
  for (const Edge* e : output_edges) {
    int src_output = e->src_output();
    int new_src_output = index_mapping.at(src_output);
    Node* dst = e->dst();
    int dst_input = e->dst_input();
    g->RemoveEdge(e);

    if (new_src_output < input_count - resource_input_count) {
      g->AddEdge(n, new_src_output, dst, dst_input);
    } else {
      const Edge* input_edge;
      TF_RETURN_IF_ERROR(n->input_edge(new_src_output, &input_edge));
      g->AddEdge(input_edge->src(), input_edge->src_output(), dst, dst_input);
    }
  }
  return Status::OK();
}

// Given mapping between original input index and rearranged input index, change
// "index" attribute for _Arg nodes.
void RearrangeArgNodes(gtl::InlinedVector<Node*, 4>* arg_nodes,  // non-absl ok
                       const std::vector<int>& index_mapping) {
  for (int i = 0; i < arg_nodes->size(); i++) {
    Node* n = (*arg_nodes)[i];
    int new_index = index_mapping.at(i);
    n->ClearAttr("index");
    n->AddAttr("index", new_index);
  }
}

// Given all _Retval nodes in the function, return if we need to rewrite the
// function (by checking if we have DT_RESOURCE return values). If we need to
// rewrite the function, `retval_index_mapping` will hold the mapping from
// original _Retval to rearranged _Retval, and `resource_retval_to_arg` will
// hold mapping from DT_RESOURCE _Retval index to its input _Arg index. Here we
// assume that all DT_RESOURCE _Retval nodes come from _Arg nodes directly.
Status CalculateRetvalRearrange(
    const gtl::InlinedVector<Node*, 4>& ret_nodes,  // non-absl ok
    std::map<int, int>* retval_index_mapping,
    std::map<int, int>* resource_retval_to_arg) {
  for (int i = 0; i < ret_nodes.size(); i++) {
    Node* n = ret_nodes[i];
    DataType t;
    TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), "T", &t));
    if (t != DT_RESOURCE) {
      int new_retval_index = retval_index_mapping->size();
      retval_index_mapping->insert(std::make_pair(i, new_retval_index));
      continue;
    }

    const Edge* e;
    TF_RETURN_IF_ERROR(n->input_edge(0, &e));
    if (!e->src()->IsArg()) {
      return errors::Unimplemented(
          "Resource _Retval node's input does not come from _Arg "
          "directly: ",
          e->DebugString());
    }
    Node* arg = e->src();
    int src_index;
    TF_RETURN_IF_ERROR(GetNodeAttr(arg->def(), "index", &src_index));
    resource_retval_to_arg->insert(std::make_pair(i, src_index));
  }
  return Status::OK();
}

// Given original output types and return value index mapping, return the new
// output types. Notice that DT_RESOURCE will be removed.
std::vector<DataType> ShuffleOutputDataTypeAttribute(
    const std::vector<DataType>& out_types,
    const std::map<int, int>& index_mapping) {
  std::vector<DataType> result(index_mapping.size());
  for (int i = 0; i < out_types.size(); i++) {
    auto iter = index_mapping.find(i);
    if (iter != index_mapping.end()) {
      result[iter->second] = out_types[i];
    }
  }
  return result;
}

// For StatefulPartitionedCall node, given mapping between original input index
// and rearranged input index, reorder output edges for the node. DT_RESOURCE
// outputs are removed from the node and we will use the node's corresponding
// input for the edge.
Status RearrangeOutputEdges(Node* n, Graph* g,
                            const std::map<int, int>& retval_index_mapping,
                            const std::map<int, int>& resource_retval_to_arg) {
  std::vector<const Edge*> out_edges;
  for (const Edge* e : n->out_edges()) {
    if (!e->IsControlEdge()) {
      out_edges.push_back(e);
    }
  }
  for (const Edge* e : out_edges) {
    Node* dst = e->dst();
    int dst_input = e->dst_input();
    int src_output = e->src_output();
    auto iter = retval_index_mapping.find(src_output);
    if (iter == retval_index_mapping.end()) {
      TF_RET_CHECK(resource_retval_to_arg.find(src_output) !=
                   resource_retval_to_arg.end());
      g->RemoveEdge(e);
      const Edge* input_edge;
      TF_RETURN_IF_ERROR(
          n->input_edge(resource_retval_to_arg.at(src_output), &input_edge));
      g->AddEdge(input_edge->src(), input_edge->src_output(), dst, dst_input);
    } else {
      g->RemoveEdge(e);
      g->AddEdge(n, iter->second, dst, dst_input);
    }
  }
  return Status::OK();
}

// Given mapping between original output index and rearranged output index,
// change "index" attribute for _Retval nodes. Notice that DT_RESOURCE _Retval
// nodes will be removed.
void RearrangeRetvalNodes(
    const gtl::InlinedVector<Node*, 4>& ret_nodes,  // non-absl ok
    Graph* g, const std::map<int, int>& retval_index_mapping) {
  for (int i = 0; i < ret_nodes.size(); i++) {
    Node* n = ret_nodes[i];
    auto iter = retval_index_mapping.find(i);
    if (iter == retval_index_mapping.end()) {
      g->RemoveNode(n);
    } else {
      n->ClearAttr("index");
      n->AddAttr("index", iter->second);
    }
  }
}

Status MaybeRewriteWhileNode(Graph* g, Node* n, FunctionLibraryDefinition* fld,
                             bool* node_rewritten) {
  // Check if this While node needs rewrite.
  std::vector<DataType> types;
  TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), "T", &types));
  bool input_need_rearrange;
  int resource_input_count;
  std::vector<int> index_mapping;
  TF_RETURN_IF_ERROR(InputTypesNeedsRearrange(
      types, &input_need_rearrange, &resource_input_count, &index_mapping));
  if (!input_need_rearrange) {
    *node_rewritten = false;
    return Status::OK();
  }

  *node_rewritten = true;

  // Modify "T" attribute for this While node.
  std::vector<DataType> new_types =
      ShuffleInputDataTypeAttribute(types, index_mapping);
  n->ClearAttr("T");
  n->AddAttr("T", new_types);

  // Reorder input and output edges.
  TF_RETURN_IF_ERROR(ReorderInputEdges(g, n, index_mapping));
  TF_RETURN_IF_ERROR(ReorderOutputEdges(g, n, types.size(),
                                        resource_input_count, index_mapping));

  // Modify cond and body functions.
  for (auto const& attr_name : std::vector<string>{"cond", "body"}) {
    NameAttrList attr_value;
    TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), attr_name, &attr_value));
    const FunctionDef* fdef = fld->Find(attr_value.name());
    TF_RET_CHECK(fdef != nullptr);
    std::unique_ptr<FunctionBody> fbody;
    TF_RETURN_IF_ERROR(
        FunctionDefToBodyHelper(*fdef, AttrSlice(), fld, &fbody));

    // Check that resource _Arg nodes for While node are always returned with
    // the same index, and we don't have cases like this:
    // tf.while_loop(
    //     cond,
    //     lambda resource_var1, resource_var2: [resource_var2, resource_var1],
    //     [resource_var1, resource_var2])
    if (attr_name == "body") {
      for (int i = 0; i < fbody->ret_nodes.size(); i++) {
        Node* n = fbody->ret_nodes[i];
        DataType dtype;
        TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), "T", &dtype));
        if (dtype != DT_RESOURCE) {
          continue;
        }

        Node* input_node;
        TF_RETURN_IF_ERROR(n->input_node(0, &input_node));
        while (input_node->IsIdentity()) {
          TF_RETURN_IF_ERROR(input_node->input_node(0, &input_node));
        }
        if (input_node->IsArg()) {
          int index;
          TF_RETURN_IF_ERROR(GetNodeAttr(input_node->def(), "index", &index));
          if (index != i) {
            return errors::Unimplemented("While node ", n->DebugString(),
                                         " has resource _Retval[", i,
                                         "] coming from _Arg[", index, "]");
          }
        } else {
          return errors::Unimplemented("Encountered node ",
                                       input_node->DebugString(),
                                       " while tracing _Arg node for _Retval[",
                                       i, "] of while node ", n->DebugString());
        }
      }
    }

    RearrangeArgNodes(&fbody->arg_nodes, index_mapping);
    if (attr_name == "body") {
      for (int i = 0; i < fbody->ret_nodes.size(); i++) {
        Node* n = fbody->ret_nodes[i];
        int new_index = index_mapping.at(i);
        if (new_index < types.size() - resource_input_count) {
          n->ClearAttr("index");
          n->AddAttr("index", new_index);
        } else {
          fbody->graph->RemoveNode(n);
        }
      }
    }

    // Save the new FunctionDef.
    FunctionDef new_fdef;
    string new_name =
        fld->UniqueFunctionName(absl::StrCat(attr_value.name(), "_rearrange_"));
    TF_RETURN_IF_ERROR(GraphToFunctionDef(*fbody->graph, new_name, &new_fdef));
    TF_RETURN_IF_ERROR(fld->AddFunctionDef(new_fdef));

    // Change node to use rewritten function.
    attr_value.set_name(new_name);
    n->ClearAttr(attr_name);
    n->AddAttr(attr_name, attr_value);
  }
  return Status::OK();
}

Status MaybeRewriteCallNode(Graph* g, Node* n, FunctionLibraryDefinition* fld,
                            bool* node_rewritten) {
  // This node needs rewrite when either of these is true:
  // 1) Tin has DT_RESOURCE which requires rearrange;
  // 2) Tout has DT_RESOURCE.
  std::vector<DataType> in_types;
  TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), "Tin", &in_types));
  bool input_need_rearrange;
  int resource_input_count;
  std::vector<int> index_mapping;
  TF_RETURN_IF_ERROR(InputTypesNeedsRearrange(
      in_types, &input_need_rearrange, &resource_input_count, &index_mapping));
  std::vector<DataType> out_types;
  TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), "Tout", &out_types));
  bool has_resource_output = std::find(out_types.begin(), out_types.end(),
                                       DT_RESOURCE) != out_types.end();
  if (!resource_input_count && !has_resource_output) {
    *node_rewritten = false;
    return Status::OK();
  }

  *node_rewritten = true;

  string attr_name = "f";
  NameAttrList f;
  TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), attr_name, &f));
  const FunctionDef* fdef = fld->Find(f.name());
  TF_RET_CHECK(fdef != nullptr);
  std::unique_ptr<FunctionBody> fbody;
  TF_RETURN_IF_ERROR(FunctionDefToBodyHelper(*fdef, AttrSlice(), fld, &fbody));

  if (input_need_rearrange) {
    // Reorder input edges.
    TF_RETURN_IF_ERROR(ReorderInputEdges(g, n, index_mapping));

    // Change Tin attribute.
    std::vector<DataType> new_in_types =
        ShuffleInputDataTypeAttribute(in_types, index_mapping);
    n->ClearAttr("Tin");
    n->AddAttr("Tin", new_in_types);

    // Change _Arg node index.
    RearrangeArgNodes(&fbody->arg_nodes, index_mapping);
  }

  if (has_resource_output) {
    // Resource _Retval must come from resource _Arg directly, or we do not
    // support it.
    std::map<int, int> resource_retval_to_arg, retval_index_mapping;
    TF_RETURN_IF_ERROR(CalculateRetvalRearrange(
        fbody->ret_nodes, &retval_index_mapping, &resource_retval_to_arg));

    // Rearrange output edges.
    TF_RETURN_IF_ERROR(RearrangeOutputEdges(n, g, retval_index_mapping,
                                            resource_retval_to_arg));

    // Change Tout attribute for the node.
    std::vector<DataType> new_out_types =
        ShuffleOutputDataTypeAttribute(out_types, retval_index_mapping);
    n->ClearAttr("Tout");
    n->AddAttr("Tout", new_out_types);

    // Change index for _Retval nodes.
    RearrangeRetvalNodes(fbody->ret_nodes, fbody->graph, retval_index_mapping);
  }

  // Save the new FunctionDef.
  FunctionDef new_fdef;
  string new_name =
      fld->UniqueFunctionName(absl::StrCat(f.name(), "_rearrange_"));
  TF_RETURN_IF_ERROR(GraphToFunctionDef(*fbody->graph, new_name, &new_fdef));
  TF_RETURN_IF_ERROR(fld->AddFunctionDef(new_fdef));

  // Change node to use rewritten function.
  f.set_name(new_name);
  n->ClearAttr(attr_name);
  n->AddAttr(attr_name, f);
  return Status::OK();
}

Status MaybeRewriteIfNode(Graph* g, Node* n, FunctionLibraryDefinition* fld,
                          bool* node_rewritten) {
  // This node needs rewrite when either of these is true:
  // 1) Tin has DT_RESOURCE which requires rearrange;
  // 2) Tout has DT_RESOURCE.
  std::vector<DataType> in_types;
  TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), "Tin", &in_types));
  bool input_need_rearrange;
  int resource_input_count;
  std::vector<int> index_mapping;
  TF_RETURN_IF_ERROR(InputTypesNeedsRearrange(
      in_types, &input_need_rearrange, &resource_input_count, &index_mapping));
  std::vector<DataType> out_types;
  TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), "Tout", &out_types));
  bool has_resource_output = std::find(out_types.begin(), out_types.end(),
                                       DT_RESOURCE) != out_types.end();
  if (!input_need_rearrange && !has_resource_output) {
    *node_rewritten = false;
    return Status::OK();
  }

  *node_rewritten = true;

  if (input_need_rearrange) {
    // Reorder input edges.
    std::vector<const Edge*> input_edges;
    for (const Edge* e : n->in_edges()) {
      if (e->IsControlEdge() || e->dst_input() == 0) {
        continue;
      }
      input_edges.push_back(e);
    }
    for (const Edge* e : input_edges) {
      Node* src = e->src();
      int src_output = e->src_output();
      int dst_input = e->dst_input();
      int new_dst_input = index_mapping.at(dst_input - 1) + 1;
      g->RemoveEdge(e);
      g->AddEdge(src, src_output, n, new_dst_input)->DebugString();
    }

    // Change Tin attribute.
    std::vector<DataType> new_in_types =
        ShuffleInputDataTypeAttribute(in_types, index_mapping);
    n->ClearAttr("Tin");
    n->AddAttr("Tin", new_in_types);
  }

  std::map<int, int> resource_retval_to_arg, retval_index_mapping;
  for (auto const& attr_name :
       std::vector<string>{"then_branch", "else_branch"}) {
    NameAttrList f;
    TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), attr_name, &f));
    const FunctionDef* fdef = fld->Find(f.name());
    TF_RET_CHECK(fdef != nullptr);
    std::unique_ptr<FunctionBody> fbody;
    TF_RETURN_IF_ERROR(
        FunctionDefToBodyHelper(*fdef, AttrSlice(), fld, &fbody));

    if (input_need_rearrange) {
      // Change _Arg node index.
      RearrangeArgNodes(&fbody->arg_nodes, index_mapping);
    }

    if (has_resource_output) {
      // Resource _Retval must come from resource _Arg directly, or we do
      // not support it.
      TF_RETURN_IF_ERROR(CalculateRetvalRearrange(
          fbody->ret_nodes, &retval_index_mapping, &resource_retval_to_arg));

      // Change index for _Retval nodes.
      RearrangeRetvalNodes(fbody->ret_nodes, fbody->graph,
                           retval_index_mapping);
    }

    // Save the new FunctionDef.
    FunctionDef new_fdef;
    string new_name =
        fld->UniqueFunctionName(absl::StrCat(f.name(), "_rearrange_"));
    TF_RETURN_IF_ERROR(GraphToFunctionDef(*fbody->graph, new_name, &new_fdef));
    TF_RETURN_IF_ERROR(fld->AddFunctionDef(new_fdef));

    // Change node to use rewritten function.
    f.set_name(new_name);
    n->ClearAttr(attr_name);
    n->AddAttr(attr_name, f);
  }

  if (has_resource_output) {
    // Rearrange output edges.
    std::vector<const Edge*> out_edges;
    for (const Edge* e : n->out_edges()) {
      if (!e->IsControlEdge()) {
        out_edges.push_back(e);
      }
    }
    for (const Edge* e : out_edges) {
      Node* dst = e->dst();
      int dst_input = e->dst_input();
      int src_output = e->src_output();
      auto iter = retval_index_mapping.find(src_output);
      if (iter == retval_index_mapping.end()) {
        TF_RET_CHECK(resource_retval_to_arg.find(src_output) !=
                     resource_retval_to_arg.end());
        g->RemoveEdge(e);
        const Edge* input_edge;
        TF_RETURN_IF_ERROR(n->input_edge(
            resource_retval_to_arg.at(src_output) + 1, &input_edge));
        g->AddEdge(input_edge->src(), input_edge->src_output(), dst, dst_input);
      } else {
        g->RemoveEdge(e);
        g->AddEdge(n, iter->second, dst, dst_input);
      }
    }

    // Change Tout attribute for the node.
    std::vector<DataType> new_out_types =
        ShuffleOutputDataTypeAttribute(out_types, retval_index_mapping);
    n->ClearAttr("Tout");
    n->AddAttr("Tout", new_out_types);
  }
  return Status::OK();
}

}  // namespace

Status RearrangeFunctionArgumentForFunction(
    const string& func_name, const string& new_func_name,
    const protobuf::Map<string, tensorflow::AttrValue>& attrs,
    FunctionLibraryDefinition* fld, FunctionLibraryRuntime* flr,
    std::map<string, absl::optional<string>>* canonicalized_name_to_new_name,
    bool* modified) {
  *modified = false;

  // Convert the function to Graph.
  FunctionLibraryRuntime::Handle handle;
  TF_RETURN_IF_ERROR(flr->Instantiate(func_name, AttrSlice(&attrs), &handle));
  Status ret_status = Status::OK();
  auto cleanup_handle = gtl::MakeCleanup([&]() {
    auto s = flr->ReleaseHandle(handle);
    if (!s.ok()) {
      ret_status.Update(s);
    }
  });
  const FunctionBody* body = flr->GetFunctionBody(handle);
  Graph* g = body->graph;

  // If any node has associated functions, rewrite them first.
  // Gather nodes with associated functions first, because rewriting those nodes
  // might involve node deletion/addition. Avoid modifying nodes while iterating
  // it.
  std::vector<std::pair<Node*, std::vector<AssociatedFunctionInfo>>>
      nodes_to_associated_functions;
  for (auto* n : g->nodes()) {
    auto associated_functions = GetAssociatedFunctions(*n, fld);
    if (!associated_functions.empty()) {
      nodes_to_associated_functions.push_back({n, associated_functions});
    }
  }
  for (auto iter : nodes_to_associated_functions) {
    Node* n = iter.first;
    auto associated_functions = iter.second;
    for (auto& associated_function : associated_functions) {
      string name = associated_function.func_name();
      string canonicalized_name =
          Canonicalize(name, AttrSlice(&associated_function.attrs()));
      auto iter = canonicalized_name_to_new_name->find(canonicalized_name);
      string new_name;
      bool function_modified;
      if (iter != canonicalized_name_to_new_name->end()) {
        // If we already processed this function, check if it was rewritten. If
        // the function was rewritten, the entry will be non-empty. Otherwise
        // the entry will be empty.
        function_modified = iter->second.has_value();
        if (function_modified) {
          new_name = iter->second.value();
        }
      } else {
        if (associated_function.type() ==
            AssociatedFunctionInfo::AssociatedFunctionType::kSymbolicGradient) {
          // For SymbolicGradient, `name` is always "SymbolicGradient",
          // which is not very informative. Use node name instead.
          new_name =
              fld->UniqueFunctionName(absl::StrCat(n->name(), "_rearrange_"));
        } else {
          new_name = fld->UniqueFunctionName(absl::StrCat(name, "_rearrange_"));
        }
        TF_RETURN_IF_ERROR(RearrangeFunctionArgumentForFunction(
            name, new_name, associated_function.attrs(), fld, flr,
            canonicalized_name_to_new_name, &function_modified));
        if (function_modified) {
          // If the function was rewritten, add an non-empty entry. So later we
          // know we have processed this function, and it was rewritten into
          // another function.
          (*canonicalized_name_to_new_name)[canonicalized_name] = new_name;
        } else {
          // If the function was not rewritten, add an empty entry. So later
          // we know we have processed this function, and it does not need to be
          // rewritten.
          (*canonicalized_name_to_new_name)[canonicalized_name] = absl::nullopt;
        }
      }
      if (function_modified) {
        *modified = true;

        // Notice that if "n" is a function call, RewriteAssociatedFunction()
        // will delete it and create a new node instead, making "n" an invalid
        // pointer. That's fine because in that case, associated_functions will
        // only have one member and the loop will only run once.
        TF_RETURN_IF_ERROR(RewriteAssociatedFunction(
            g, n, fld, associated_function, new_name));
      }
    }
  }

  for (Node* n : g->nodes()) {
    if (n->type_string() == "While") {
      bool node_rewritten;
      TF_RETURN_IF_ERROR(MaybeRewriteWhileNode(g, n, fld, &node_rewritten));
      if (node_rewritten) {
        *modified = true;
      }
    } else if (n->type_string() == "StatefulPartitionedCall") {
      bool node_rewritten;
      TF_RETURN_IF_ERROR(MaybeRewriteCallNode(g, n, fld, &node_rewritten));
      if (node_rewritten) {
        *modified = true;
      }
    } else if (n->type_string() == "If") {
      bool node_rewritten;
      TF_RETURN_IF_ERROR(MaybeRewriteIfNode(g, n, fld, &node_rewritten));
      if (node_rewritten) {
        *modified = true;
      }
    }
  }

  if (*modified) {
    // Add rewritten FunctionDef into library.
    FunctionDef functionalized_fdef;
    TF_RETURN_IF_ERROR(
        GraphToFunctionDef(*g, new_func_name, &functionalized_fdef));
    if (func_name == new_func_name) {
      VLOG(2) << "Replacing function " << func_name;
      TF_RETURN_IF_ERROR(
          fld->ReplaceFunction(new_func_name, functionalized_fdef));
    } else {
      VLOG(2) << "Adding function " << new_func_name;
      TF_RETURN_IF_ERROR(fld->AddFunctionDef(functionalized_fdef));
    }
  }

  return ret_status;
}  // namespace tensorflow

Status RearrangeFunctionArgumentPass::Run(
    const GraphOptimizationPassOptions& options) {
  Graph* graph = options.graph->get();
  if (VLOG_IS_ON(4)) {
    DumpGraphToFile("rearrange_function_argument_before", *graph,
                    options.flib_def);
  }
  std::unique_ptr<ProcessFunctionLibraryRuntime> pflr(
      new ProcessFunctionLibraryRuntime(
          /*device_mgr=*/nullptr, options.session_options->env,
          TF_GRAPH_DEF_VERSION, options.flib_def, OptimizerOptions()));
  FunctionLibraryRuntime* flr =
      pflr->GetFLR(ProcessFunctionLibraryRuntime::kDefaultFLRDevice);

  // Find XLA compile ops and its corresponding FunctionDef.
  static std::map<string, string>* kNodeTypeToFunctionAttrMapping =
      new std::map<string, string>{
          // TPUReplicate ops are generated by EncapsulateTPUComputationsPass.
          {"TPUReplicate", "computation"},
          // XlaLaunch ops are generated by EncapsulateXlaComputationsPass.
          {"XlaLaunch", "function"},
      };
  std::map<string, absl::optional<string>> canonicalized_name_to_new_name;
  bool fld_modified = false;
  for (Node* n : graph->nodes()) {
    auto it = kNodeTypeToFunctionAttrMapping->find(n->type_string());
    if (it == kNodeTypeToFunctionAttrMapping->end()) {
      continue;
    }
    const string func_attr = it->second;
    NameAttrList func;
    TF_RETURN_IF_ERROR(GetNodeAttr(n->attrs(), func_attr, &func));
    VLOG(2) << "Graph has node " << n->type_string()
            << ". Corresponding function: " << func.name();
    string new_func_name = options.flib_def->UniqueFunctionName(
        absl::StrCat(func.name(), "_rearrange_"));
    bool modified = false;
    TF_RETURN_IF_ERROR(RearrangeFunctionArgumentForFunction(
        func.name(), new_func_name, func.attr(), options.flib_def, flr,
        &canonicalized_name_to_new_name, &modified));
    if (modified) {
      n->ClearAttr(func_attr);
      func.set_name(new_func_name);
      n->AddAttr(func_attr, func);

      fld_modified = true;
    }
  }
  if (fld_modified) {
    TF_RETURN_IF_ERROR(
        PruneUnreachableFunctionsFromGraph(**options.graph, options.flib_def));
  }

  if (VLOG_IS_ON(4)) {
    DumpGraphToFile("rearrange_function_argument_after", *graph,
                    options.flib_def);
  }
  return Status::OK();
}

}  // namespace tensorflow
