/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CallGraph.h"

#include <utility>

#include "ConcurrentContainers.h"
#include "MethodOverrideGraph.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace call_graph {

Graph single_callee_graph(const Scope& scope) {
  return Graph(SingleCalleeStrategy(scope));
}

Graph complete_call_graph(const Scope& scope) {
  return Graph(CompleteCallGraphStrategy(scope));
}

Graph multiple_callee_graph(const Scope& scope,
                            uint32_t big_override_threshold) {
  return Graph(MultipleCalleeStrategy(scope, big_override_threshold));
}

SingleCalleeStrategy::SingleCalleeStrategy(const Scope& scope)
    : m_scope(scope) {
  auto non_virtual_vec = mog::get_non_true_virtuals(scope);
  m_non_virtual.insert(non_virtual_vec.begin(), non_virtual_vec.end());
}

CallSites SingleCalleeStrategy::get_callsites(const DexMethod* method) const {
  CallSites callsites;
  auto* code = const_cast<IRCode*>(method->get_code());
  if (code == nullptr) {
    return callsites;
  }
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (opcode::is_an_invoke(insn->opcode())) {
      auto callee = this->resolve_callee(method, insn);
      if (callee == nullptr || is_definitely_virtual(callee)) {
        continue;
      }
      if (callee->is_concrete()) {
        callsites.emplace_back(callee, code->iterator_to(mie));
      }
    }
  }
  return callsites;
}

std::vector<const DexMethod*> SingleCalleeStrategy::get_roots() const {
  std::vector<const DexMethod*> roots;

  walk::code(m_scope, [&](DexMethod* method, IRCode& /* code */) {
    if (is_definitely_virtual(method) || root(method) ||
        method::is_clinit(method)) {
      roots.emplace_back(method);
    }
  });
  return roots;
}

bool SingleCalleeStrategy::is_definitely_virtual(DexMethod* method) const {
  return method->is_virtual() && m_non_virtual.count(method) == 0;
}

DexMethod* SingleCalleeStrategy::resolve_callee(const DexMethod* caller,
                                                IRInstruction* invoke) const {
  return resolve_method(
      invoke->get_method(), opcode_to_search(invoke), m_resolved_refs, caller);
}

MultipleCalleeBaseStrategy::MultipleCalleeBaseStrategy(const Scope& scope)
    : SingleCalleeStrategy(scope),
      m_method_override_graph(mog::build_graph(scope)) {}

std::vector<const DexMethod*> MultipleCalleeBaseStrategy::get_roots() const {
  std::vector<const DexMethod*> roots;
  MethodSet emplaced_methods;
  // Gather clinits and root methods, and the methods that override or
  // overriden by the root methods.
  auto add_root_method_overrides = [&](const DexMethod* method) {
    if (!method->get_code() || root(method) || method->is_external()) {
      // No need to add root methods, they will be added anyway.
      return;
    }
    if (!emplaced_methods.count(method)) {
      roots.emplace_back(method);
      emplaced_methods.emplace(method);
    }
  };
  walk::methods(m_scope, [&](DexMethod* method) {
    if (method::is_clinit(method)) {
      roots.emplace_back(method);
      emplaced_methods.emplace(method);
      return;
    }
    if (!root(method) && !(method->is_virtual() &&
                           is_interface(type_class(method->get_class())) &&
                           !can_rename(method))) {
      // For root methods and dynamically added classes, created via
      // Proxy.newProxyInstance, we need to add them and their overrides and
      // overriden to roots.
      return;
    }
    if (!emplaced_methods.count(method)) {
      roots.emplace_back(method);
      emplaced_methods.emplace(method);
    }
    const auto& overriding_methods =
        mog::get_overriding_methods(*m_method_override_graph, method);
    for (auto overriding_method : overriding_methods) {
      add_root_method_overrides(overriding_method);
    }
    const auto& overiden_methods =
        mog::get_overridden_methods(*m_method_override_graph, method);
    for (auto overiden_method : overiden_methods) {
      add_root_method_overrides(overiden_method);
    }
  });
  // Gather methods that override or implement external methods as well.
  for (auto& pair : m_method_override_graph->nodes()) {
    auto method = pair.first;
    if (!method->is_external()) {
      continue;
    }
    const auto& overriding_methods =
        mog::get_overriding_methods(*m_method_override_graph, method);
    for (auto* overriding : overriding_methods) {
      if (!overriding->is_external() && !emplaced_methods.count(overriding)) {
        roots.emplace_back(overriding);
        emplaced_methods.emplace(overriding);
      }
    }
  }
  // Add additional roots if needed.
  auto additional_roots = get_additional_roots(emplaced_methods);
  roots.insert(roots.end(), additional_roots.begin(), additional_roots.end());
  return roots;
}

CompleteCallGraphStrategy::CompleteCallGraphStrategy(const Scope& scope)
    : MultipleCalleeBaseStrategy(scope) {}

CallSites CompleteCallGraphStrategy::get_callsites(
    const DexMethod* method) const {
  CallSites callsites;
  auto* code = const_cast<IRCode*>(method->get_code());
  if (code == nullptr) {
    return callsites;
  }
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (opcode::is_an_invoke(insn->opcode())) {
      auto callee = this->resolve_callee(method, insn);
      if (callee == nullptr) {
        continue;
      }
      if (callee->is_concrete()) {
        callsites.emplace_back(callee, code->iterator_to(mie));
      }
      auto overriding =
          mog::get_overriding_methods(*m_method_override_graph, callee);

      for (auto m : overriding) {
        callsites.emplace_back(m, code->iterator_to(mie));
      }
    }
  }
  return callsites;
}

std::vector<const DexMethod*> CompleteCallGraphStrategy::get_roots() const {
  std::vector<const DexMethod*> roots;

  walk::methods(m_scope, [&](DexMethod* method) {
    if (root(method) || method::is_clinit(method)) {
      roots.emplace_back(method);
    }
  });
  return roots;
}

MultipleCalleeStrategy::MultipleCalleeStrategy(const Scope& scope,
                                               uint32_t big_override_threshold)
    : MultipleCalleeBaseStrategy(scope) {
  // Gather big overrides true virtual methods.
  ConcurrentSet<const DexMethod*> bigoverrides;
  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (opcode::is_an_invoke(insn->opcode())) {
        auto callee =
            resolve_method(insn->get_method(), opcode_to_search(insn), method);
        if (callee == nullptr || !callee->is_virtual()) {
          continue;
        }
        const auto& overriding_methods =
            mog::get_overriding_methods(*m_method_override_graph, callee);
        uint32_t num_override = 0;
        for (auto overriding_method : overriding_methods) {
          if (overriding_method->get_code()) {
            ++num_override;
          }
        }
        if (num_override > big_override_threshold) {
          bigoverrides.emplace(callee);
          for (auto overriding_method : overriding_methods) {
            bigoverrides.emplace(overriding_method);
          }
        }
      }
    }
  });
  for (auto item : bigoverrides) {
    m_big_override.emplace(item);
  }
}

CallSites MultipleCalleeStrategy::get_callsites(const DexMethod* method) const {
  CallSites callsites;
  auto* code = const_cast<IRCode*>(method->get_code());
  if (code == nullptr) {
    return callsites;
  }
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (opcode::is_an_invoke(insn->opcode())) {
      auto callee = this->resolve_callee(method, insn);
      if (callee == nullptr) {
        continue;
      }
      if (is_definitely_virtual(callee)) {
        // For true virtual callees, add the callee itself and all of its
        // overrides if they are not in big overrides.
        if (m_big_override.count(callee)) {
          continue;
        }
        if (callee->get_code()) {
          callsites.emplace_back(callee, code->iterator_to(mie));
        }
        if (insn->opcode() != OPCODE_INVOKE_SUPER) {
          const auto& overriding_methods =
              mog::get_overriding_methods(*m_method_override_graph, callee);
          for (auto overriding_method : overriding_methods) {
            callsites.emplace_back(overriding_method, code->iterator_to(mie));
          }
        }
      } else if (callee->is_concrete()) {
        callsites.emplace_back(callee, code->iterator_to(mie));
      }
    }
  }
  return callsites;
}

// Add big override methods to root as well.
std::vector<const DexMethod*> MultipleCalleeStrategy::get_additional_roots(
    const MethodSet& existing_roots) const {
  std::vector<const DexMethod*> additional_roots;
  for (auto method : m_big_override) {
    if (!method->is_external() && !existing_roots.count(method)) {
      additional_roots.emplace_back(method);
    }
  }
  return additional_roots;
}

Edge::Edge(NodeId caller, NodeId callee, const IRList::iterator& invoke_it)
    : m_caller(std::move(caller)),
      m_callee(std::move(callee)),
      m_invoke_it(invoke_it) {}

Graph::Graph(const BuildStrategy& strat)
    : m_entry(std::make_shared<Node>(Node::GHOST_ENTRY)),
      m_exit(std::make_shared<Node>(Node::GHOST_EXIT)) {
  // Add edges from the single "ghost" entry node to all the "real" entry
  // nodes in the graph.
  auto roots = strat.get_roots();
  for (const DexMethod* root : roots) {
    auto edge =
        std::make_shared<Edge>(m_entry, make_node(root), IRList::iterator());
    m_entry->m_successors.emplace_back(edge);
    make_node(root)->m_predecessors.emplace_back(edge);
  }

  // Obtain the callsites of each method recursively, building the graph in the
  // process.
  MethodSet visited;
  auto visit = [&](const auto* caller) {
    auto visit_impl = [&](const auto* caller, auto& visit_fn) {
      if (visited.count(caller) != 0) {
        return;
      }
      visited.emplace(caller);
      auto callsites = strat.get_callsites(caller);
      if (callsites.empty()) {
        this->add_edge(make_node(caller), m_exit, IRList::iterator());
      }
      for (const auto& callsite : callsites) {
        this->add_edge(
            make_node(caller), make_node(callsite.callee), callsite.invoke);
        visit_fn(callsite.callee, visit_fn);
      }
    };
    visit_impl(caller, visit_impl);
  };

  for (const DexMethod* root : roots) {
    visit(root);
  }
}

NodeId Graph::make_node(const DexMethod* m) {
  auto it = m_nodes.find(m);
  if (it != m_nodes.end()) {
    return it->second;
  }
  m_nodes.emplace(m, std::make_shared<Node>(m));
  return m_nodes.at(m);
}

void Graph::add_edge(const NodeId& caller,
                     const NodeId& callee,
                     const IRList::iterator& invoke_it) {
  auto edge = std::make_shared<Edge>(caller, callee, invoke_it);
  caller->m_successors.emplace_back(edge);
  callee->m_predecessors.emplace_back(edge);
}

MethodSet resolve_callees_in_graph(const Graph& graph,
                                   const DexMethod* method,
                                   const IRInstruction* insn) {
  MethodSet ret;
  for (const auto& edge_id : graph.node(method)->callees()) {
    auto it = edge_id->invoke_iterator();
    if (it != IRList::iterator() && it->insn == insn) {
      auto callee_node_id = edge_id->callee();
      if (callee_node_id) {
        auto callee = callee_node_id->method();
        if (callee) {
          ret.emplace(callee);
        }
      }
    }
  }
  return ret;
}

CallgraphStats get_num_nodes_edges(const Graph& graph) {
  std::unordered_set<NodeId> visited_node;
  std::queue<NodeId> to_visit;
  uint32_t num_edge = 0;
  uint32_t num_callsites = 0;
  to_visit.push(graph.entry());
  while (!to_visit.empty()) {
    auto front = to_visit.front();
    to_visit.pop();
    if (!visited_node.count(front)) {
      visited_node.emplace(front);
      num_edge += front->callees().size();
      std::unordered_set<IRInstruction*> callsites;
      for (const auto& edge : front->callees()) {
        to_visit.push(edge->callee());
        auto it = edge->invoke_iterator();
        if (it != IRList::iterator() && !callsites.count(it->insn)) {
          callsites.emplace(it->insn);
        }
      }
      num_callsites += callsites.size();
    }
  }
  return CallgraphStats(visited_node.size(), num_edge, num_callsites);
}

} // namespace call_graph
