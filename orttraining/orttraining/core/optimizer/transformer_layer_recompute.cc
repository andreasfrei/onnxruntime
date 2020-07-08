// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/core/optimizer/transformer_layer_recompute.h"

#include <deque>

namespace onnxruntime {

std::vector<const NodeArg*> TransformerLayerRecompute::IdentifyTransformerLayerEdges(Graph& graph) const {
  std::vector<const NodeArg*> layer_edges;

  GraphViewer graph_viewer(graph);
  const auto& node_topology_list = graph_viewer.GetNodesInTopologicalOrder();
  for (auto node_index : node_topology_list) {
    auto& node = *graph.GetNode(node_index);

    if ((node.OpType() == "LayerNormalization" || node.OpType() == "TrainableDropout") && node.GetOutputEdgesCount() == 4) {
      layer_edges.push_back(node.OutputDefs()[0]);
    }
  }

  // need to match two patterns here: one for start, one for end
  return layer_edges;
}

typedef std::set<const Node*, NodeCompare> NodeSet;

std::vector<const Node*> TransformerLayerRecompute::NodesBetweenEdges(Graph& graph, const NodeArg* start, const NodeArg* end) const {
  // Forward BFS from the start node
  std::vector<const Node*> start_nodes = graph.GetConsumerNodes(start->Name());
  NodeSet fw_visited(start_nodes.begin(), start_nodes.end());
  std::deque<const Node*> fw_queue(start_nodes.begin(), start_nodes.end());
  while (!fw_queue.empty()) {
    const Node* n = fw_queue.front();
    fw_queue.pop_front();

    for (auto node_it = n->OutputNodesBegin(); node_it != n->OutputNodesEnd(); ++node_it) {
      const Node& node = *node_it;
      if (fw_visited.find(&node) == fw_visited.end()) {
        fw_queue.push_back(&node);
        fw_visited.insert(&node);
      }
    }
  }

  // Reverse BFS from the end node
  const Node* end_node = graph.GetProducerNode(end->Name());
  // exclued the end_node from the bw_visited set, since end edge is preserved
  NodeSet bw_visited;
  std::deque<const Node*> bw_queue{end_node};

  while (!bw_queue.empty()) {
    const Node* n = bw_queue.front();
    bw_queue.pop_front();

    for (auto node_it = n->InputNodesBegin(); node_it != n->InputNodesEnd(); ++node_it) {
      const Node& node = *node_it;
      if (bw_visited.find(&node) == bw_visited.end()) {
        bw_queue.push_back(&node);
        bw_visited.insert(&node);
      }
    }
  }

  // Join fw_visited and bw_visited
  // TODO: consider usig std::set_intersection
  std::vector<const Node*> intersect_nodes;
  for (const Node* n : fw_visited) {
    if (bw_visited.find(n) != bw_visited.end()) {
      intersect_nodes.push_back(n);
    }
  }

  // std::cout << "start: " << start->Name() << "\n";
  // std::cout << "end: " << end->Name() << "\n";

  // for (const Node* node : intersect_nodes) {
  //   std::cout << "Node: " << node->Name() << "\n";
  // }

  return intersect_nodes;
}

void TransformerLayerRecompute::InsertRecomputeNodes(Graph& graph, const std::vector<const Node*>& nodes) const {
  auto initializers = graph.GetAllInitializedTensors();

  for (const Node* n : nodes) {
    Node* node = graph.GetNode(n->Index());

    if (node->OpType() == "TrainableDropout" || node->OpType() == "Dropout") {
      std::vector<NodeArg*> recomputed_inputs;
      NodeArg* input = node->MutableInputDefs()[0];
      const Node* p_node = graph.GetProducerNode(input->Name());

      if (initializers.find(input->Name()) != initializers.end() ||
          std::find(nodes.begin(), nodes.end(), p_node) == nodes.end()) {
        recomputed_inputs.push_back(input);
        //std::cout << "original input: " << input->Name() << "\n";
      } else {
        auto& recomputed_input = graph.GetOrCreateNodeArg(input->Name() + "_recompute",
                                                          input->TypeAsProto());
        recomputed_inputs.push_back(&recomputed_input);

        //std::cout << "recomputed input: " << recomputed_input.Name() << "\n";
      }
      recomputed_inputs.push_back(node->MutableOutputDefs()[1]);
      recomputed_inputs.push_back(node->MutableInputDefs()[1]);

      const auto& output = node->OutputDefs()[0];
      auto& recomputed_output = graph.GetOrCreateNodeArg(output->Name() + "_recompute",
                                                         output->TypeAsProto());

      Node& recompute_node = graph.AddNode(node->Name() + "_recompute",
                                           "TrainableDropoutGrad",
                                           "Recompute of " + node->Name(),
                                           recomputed_inputs,
                                           {&recomputed_output},
                                           {},
                                           kMSDomain);
      recompute_node.SetPriority(-10);
      continue;
    }

    std::vector<NodeArg*> recomputed_inputs;
    for (NodeArg* input : node->MutableInputDefs()) {
      const Node* p_node = graph.GetProducerNode(input->Name());

      if (initializers.find(input->Name()) != initializers.end() ||
          std::find(nodes.begin(), nodes.end(), p_node) == nodes.end()) {
        recomputed_inputs.push_back(input);

        // std::cout << "original input: " << input->Name() << "\n";
      } else {
        auto& recomputed_input = graph.GetOrCreateNodeArg(input->Name() + "_recompute",
                                                          input->TypeAsProto());
        recomputed_inputs.push_back(&recomputed_input);

        // std::cout << "recomputed input: " << recomputed_input.Name() << "\n";
      }
    }

    std::vector<NodeArg*> recomputed_outputs;
    for (NodeArg* output : node->MutableOutputDefs()) {
      auto& recomputed_output = graph.GetOrCreateNodeArg(output->Name() + "_recompute",
                                                         output->TypeAsProto());
      recomputed_outputs.push_back(&recomputed_output);

      // std::cout << "recomputed output: " << recomputed_output.Name() << "\n";
    }

    Node& recompute_node = graph.AddNode(node->Name() + "_recompute",
                                         node->OpType(),
                                         "Recompute of " + node->Name(),
                                         recomputed_inputs,
                                         recomputed_outputs,
                                         &node->GetAttributes(),
                                         node->Domain());
    recompute_node.SetPriority(-10);

    // std::cout << "Added Node: " << node->Name() << "_recompute\n";
  }

  return;
}

Status TransformerLayerRecompute::ApplyImpl(Graph& graph, bool& modified, int /*graph_level*/, const logging::Logger& /*logger*/) const {
  std::vector<const NodeArg*> edges = IdentifyTransformerLayerEdges(graph);

  // for (const NodeArg* edge : edges) {
  //   std::cout << "Edge: " << edge->Name() << "\n";
  // }

  for (size_t i = 0; i < edges.size() - 1; ++i) {
    std::vector<const Node*> nodes = NodesBetweenEdges(graph, edges[i], edges[i + 1]);
    InsertRecomputeNodes(graph, nodes);
  }

  modified = true;
  return Status::OK();
}

}  // namespace onnxruntime