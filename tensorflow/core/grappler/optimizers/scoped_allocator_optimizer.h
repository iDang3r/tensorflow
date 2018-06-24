/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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
#ifndef TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_SCOPED_ALLOCATOR_OPTIMIZER_H_
#define TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_SCOPED_ALLOCATOR_OPTIMIZER_H_

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "tensorflow/core/grappler/optimizers/graph_optimizer.h"
#include "tensorflow/core/protobuf/rewriter_config.pb.h"

namespace tensorflow {
namespace grappler {
class Graph;
class GraphProperties;
class NodeMap;
class ScopedAllocatorOptimizer;

// An Optimizer that introduces ScopedAllocators in order to reduce data
// movement and consolidate some kinds of Ops.
class ScopedAllocatorOptimizer : public GraphOptimizer {
 public:
  explicit ScopedAllocatorOptimizer(const ScopedAllocatorOptions& opts);
  ~ScopedAllocatorOptimizer() override;

  string name() const override { return "scoped_allocator_optimizer"; }

  Status Optimize(Cluster* cluster, const GrapplerItem& item,
                  GraphDef* optimized_graph) override;

  void Feedback(Cluster* cluster, const GrapplerItem& item,
                const GraphDef& optimized_graph, double result) override {}

  // Map from an Op name to a vector of Nodes with that Op.
  typedef std::unordered_map<string, std::vector<NodeDef*>> DevOpOccurrences;
  // Map from a device name to a DevOpOccurrences map.
  typedef std::unordered_map<string, DevOpOccurrences> GraphOpOccurrences;
  typedef std::unordered_set<string> OpNameSet;

  Status ProcessGraphDef(GraphDef* graph,
                         const GraphProperties& graph_properties);

  // Populates *occs by grouping Nodes with common Ops, according to
  // their assigned devices.
  void FindOpOccurrences(GraphDef* graph, const OpNameSet& op_names,
                         GraphOpOccurrences* occs);

  // Returns a new, unused scope_id to be assigned to a ScopedAllocator that
  // will allocate num_fields (> 0) separate tensors.
  int NewScopedAllocatorId(int num_fields);

  NodeMap* node_map() { return node_map_.get(); }

  // Appends values to the attr value under name in node_def, if present.
  // If not present does an assignment.
  static void ExtendNodeAttr(StringPiece name, const std::vector<int32>& values,
                             NodeDef* node_def);

  // Class that knows how to do graph rewriting for a particular kind of Op in
  // order to take advantage of a ScopedAllocator.
  class Rewriter {
   public:
    virtual ~Rewriter() {}

    virtual Status Rewrite(ScopedAllocatorOptimizer* paopti, GraphDef* graph,
                           const string& op_name,
                           const std::vector<NodeDef*>& nodes,
                           bool* applied) = 0;

    void SetGraphProperties(const GraphProperties& graph_properties) {
      graph_properties_ = &graph_properties;
      CHECK(graph_properties_);
    }

   protected:
    const GraphProperties* graph_properties_;
  };

 private:
  Rewriter* GetRewriter(const string& op_name);

  Status OrderNodeSet(std::vector<NodeDef*>* nodes) const;

  RewriterConfig::Toggle opt_level_;
  std::unordered_set<string> nodes_to_preserve_;
  OpNameSet op_name_set_;
  std::unordered_map<string, Rewriter*> rewriters_;
  std::vector<Rewriter*> to_delete_;
  int next_sa_id_ = 1;
  std::unique_ptr<NodeMap> node_map_;
};

}  // namespace grappler
}  // namespace tensorflow
#endif  // TENSORFLOW_CORE_GRAPPLER_OPTIMIZERS_SCOPED_ALLOCATOR_OPTIMIZER_H_
