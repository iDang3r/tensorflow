/* Copyright 2015 Google Inc. All Rights Reserved.

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

#include "tensorflow/core/framework/graph_def_util.h"

#include <set>
#include <vector>

#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op_def_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/strings/strcat.h"

namespace tensorflow {

string SummarizeGraphDef(const GraphDef& graph_def) {
  string ret;
  strings::StrAppend(&ret, "versions = ",
                     graph_def.versions().ShortDebugString(), ";\n");
  for (const NodeDef& node : graph_def.node()) {
    strings::StrAppend(&ret, SummarizeNodeDef(node), ";\n");
  }
  return ret;
}

Status ValidateExternalGraphDefSyntax(const GraphDef& graph_def) {
  for (const NodeDef& node : graph_def.node()) {
    TF_RETURN_IF_ERROR(ValidateExternalNodeDefSyntax(node));
  }
  return Status::OK();
}

Status AddDefaultAttrsToGraphDef(GraphDef* graph_def,
                                 const OpRegistryInterface& op_registry,
                                 int node_offset) {
  if (node_offset > graph_def->node_size()) {
    return errors::InvalidArgument(
        "Tried to add default attrs to GraphDef "
        "starting at offset ",
        node_offset, " with total nodes in graph: ", graph_def->node_size());
  }

  Status s;
  for (int i = node_offset; i < graph_def->node_size(); ++i) {
    NodeDef* node_def = graph_def->mutable_node(i);
    const OpDef* op_def = op_registry.LookUp(node_def->op(), &s);
    if (!s.ok()) {
      return s;
    }
    AddDefaultsToNodeDef(*op_def, node_def);
  }

  return s;
}

Status RemoveNewDefaultAttrsFromGraphDef(
    GraphDef* graph_def, const OpRegistryInterface& consumer_op_registry,
    const OpRegistryInterface& producer_op_registry,
    std::set<std::pair<string, string>>* op_attr_removed) {
  Status s;
  std::vector<string> to_remove;
  for (int n = 0; n < graph_def->node_size(); ++n) {
    NodeDef* node_def = graph_def->mutable_node(n);
    const OpDef* producer_op_def =
        producer_op_registry.LookUp(node_def->op(), &s);
    if (!s.ok()) return s;
    const OpDef* consumer_op_def =
        consumer_op_registry.LookUp(node_def->op(), &s);
    if (!s.ok()) return s;

    for (const auto& attr : node_def->attr()) {
      // If the attr is not in consumer_op_def...
      if (FindAttr(attr.first, *consumer_op_def) == nullptr) {
        const OpDef::AttrDef* producer_attr_def =
            FindAttr(attr.first, *producer_op_def);
        if (producer_attr_def == nullptr) {
          return errors::InvalidArgument(
              "Attr '", attr.first, "' missing in producer's OpDef: ",
              SummarizeOpDef(*producer_op_def), " but found in node: ",
              SummarizeNodeDef(*node_def));
        }
        // ...and it has the same value as the default in producer,
        if (producer_attr_def->has_default_value() &&
            AreAttrValuesEqual(producer_attr_def->default_value(),
                               attr.second)) {
          // then we will remove it below.
          to_remove.emplace_back(attr.first);
        }
      }
    }
    // We separate identifying which attrs should be removed from
    // actually removing them to avoid invalidating the loop iterators
    // above.
    for (const string& attr_name : to_remove) {
      node_def->mutable_attr()->erase(attr_name);
      if (op_attr_removed != nullptr) {
        op_attr_removed->insert(std::make_pair(node_def->op(), attr_name));
      }
    }
    to_remove.clear();
  }

  return s;
}

Status StrippedOpListForGraph(const GraphDef& graph_def,
                              const OpRegistryInterface& op_registry,
                              OpList* stripped_op_list) {
  stripped_op_list->clear_op();

  // Collect the sorted list of op names
  std::set<string> used_ops;
  for (const auto& node : graph_def.node()) {
    used_ops.insert(node.op());
  }

  // Build the stripped op list in sorted order.
  Status status;
  for (const string& op_name : used_ops) {
    const OpDef* op = op_registry.LookUp(op_name, &status);
    if (!op) return status;
    OpDef* stripped_op = stripped_op_list->add_op();
    stripped_op->CopyFrom(*op);
    RemoveDescriptionsFromOpDef(stripped_op);
  }
  return Status::OK();
}

}  // namespace tensorflow
