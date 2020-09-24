/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalTypeAnalyzer.h"

#include "ConcurrentContainers.h"
#include "MethodOverrideGraph.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace mog = method_override_graph;

using namespace type_analyzer;

namespace {

void trace_whole_program_state(WholeProgramState& wps) {
  if (traceEnabled(TYPE, 10)) {
    std::ostringstream out;
    out << wps;
    TRACE(TYPE, 5, "[wps] aggregated whole program state");
    TRACE(TYPE, 5, "%s", out.str().c_str());
  }
}

void trace_whole_program_state_diff(const WholeProgramState& old_wps,
                                    const WholeProgramState& new_wps) {
  if (traceEnabled(TYPE, 3)) {
    TRACE(TYPE,
          3,
          "[wps] field partition diff\n%s",
          old_wps.print_field_partition_diff(new_wps).c_str());
    TRACE(TYPE,
          3,
          "[wps] method partition diff\n%s",
          old_wps.print_method_partition_diff(new_wps).c_str());
  }
}

void scan_any_init_reachables(const call_graph::Graph& cg,
                              const DexMethod* method,
                              ConcurrentSet<const DexMethod*>& reachables) {
  if (!method || method::is_any_init(method) || reachables.count(method)) {
    return;
  }
  auto code = method->get_code();
  if (!code) {
    return;
  }
  // We include all methods reachable from clinits and ctors. Even methods don't
  // access fields can indirectly consume field values through ctor calls.
  reachables.insert(method);
  TRACE(TYPE, 5, "[any init reachables] insert %s", SHOW(method));
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (!opcode::is_an_invoke(insn->opcode())) {
      continue;
    }
    auto callee_method_def =
        resolve_method(insn->get_method(), opcode_to_search(insn), method);
    if (!callee_method_def || callee_method_def->is_external() ||
        !callee_method_def->is_concrete()) {
      continue;
    }
    if (!cg.has_node(method)) {
      TRACE(
          TYPE, 5, "[any init reachables] missing node in cg %s", SHOW(method));
      continue;
    }
    auto callees = resolve_callees_in_graph(cg, method, insn);
    for (const DexMethod* callee : callees) {
      scan_any_init_reachables(cg, callee, reachables);
    }
  }
}

} // namespace

namespace type_analyzer {

namespace global {

DexTypeEnvironment env_with_params(const IRCode* code,
                                   const ArgumentTypeEnvironment& args) {

  size_t idx = 0;
  DexTypeEnvironment env;
  for (auto& mie : InstructionIterable(code->get_param_instructions())) {
    env.set(mie.insn->dest(), args.get(idx++));
  }
  return env;
}

void GlobalTypeAnalyzer::analyze_node(
    const call_graph::NodeId& node,
    ArgumentTypePartition* current_partition) const {
  const DexMethod* method = node->method();

  if (method == nullptr) {
    return;
  }
  auto code = method->get_code();
  if (code == nullptr) {
    return;
  }
  auto& cfg = code->cfg();
  auto intra_ta = get_local_analysis(method);
  const auto outgoing_edges =
      call_graph::GraphInterface::successors(m_call_graph, node);
  std::unordered_set<IRInstruction*> outgoing_insns;
  for (const auto& edge : outgoing_edges) {
    if (edge->callee() == m_call_graph.exit()) {
      continue; // ghost edge to the ghost exit node
    }
    outgoing_insns.emplace(edge->invoke_iterator()->insn);
  }
  for (auto* block : cfg.blocks()) {
    auto state = intra_ta->get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->has_method() && outgoing_insns.count(insn)) {
        ArgumentTypeEnvironment out_args;
        for (size_t i = 0; i < insn->srcs_size(); ++i) {
          out_args.set(i, state.get(insn->src(i)));
        }
        current_partition->set(insn, out_args);
      }
      intra_ta->analyze_instruction(insn, &state);
    }
  }
}

ArgumentTypePartition GlobalTypeAnalyzer::analyze_edge(
    const std::shared_ptr<call_graph::Edge>& edge,
    const ArgumentTypePartition& exit_state_at_source) const {
  ArgumentTypePartition entry_state_at_dest;
  auto it = edge->invoke_iterator();
  if (it == IRList::iterator()) {
    entry_state_at_dest.set(CURRENT_PARTITION_LABEL,
                            ArgumentTypeEnvironment::top());
  } else {
    auto insn = it->insn;
    entry_state_at_dest.set(CURRENT_PARTITION_LABEL,
                            exit_state_at_source.get(insn));
  }
  return entry_state_at_dest;
}

std::unique_ptr<local::LocalTypeAnalyzer>
GlobalTypeAnalyzer::get_local_analysis(const DexMethod* method) const {
  auto args = ArgumentTypePartition::bottom();

  if (m_call_graph.has_node(method)) {
    args = this->get_entry_state_at(m_call_graph.node(method));
  }
  return analyze_method(method,
                        this->get_whole_program_state(),
                        args.get(CURRENT_PARTITION_LABEL));
}

bool GlobalTypeAnalyzer::is_reachable(const DexMethod* method) const {
  auto args = ArgumentTypePartition::bottom();

  if (m_call_graph.has_node(method)) {
    args = this->get_entry_state_at(m_call_graph.node(method));
  }
  auto args_domain = args.get(CURRENT_PARTITION_LABEL);
  return !args_domain.is_bottom();
}

using CombinedAnalyzer =
    InstructionAnalyzerCombiner<local::ClinitFieldAnalyzer,
                                WholeProgramAwareAnalyzer,
                                local::CtorFieldAnalyzer,
                                local::RegisterTypeAnalyzer>;

std::unique_ptr<local::LocalTypeAnalyzer> GlobalTypeAnalyzer::analyze_method(
    const DexMethod* method,
    const WholeProgramState& wps,
    ArgumentTypeEnvironment args) const {
  TRACE(TYPE, 5, "[global] analyzing %s", SHOW(method));
  always_assert(method->get_code() != nullptr);
  auto& code = *method->get_code();
  // Currently, our callgraph does not include calls to non-devirtualizable
  // virtual methods. So those methods may appear unreachable despite being
  // reachable.
  if (args.is_bottom()) {
    args.set_to_top();
  } else if (!args.is_top()) {
    TRACE(TYPE, 5, "Have args for %s: %s", SHOW(method), SHOW(args));
  }

  auto env = env_with_params(&code, args);
  DexType *clinit_type{nullptr}, *ctor_type{nullptr};
  if (method::is_clinit(method)) {
    clinit_type = method->get_class();
  } else if (method::is_init(method)) {
    ctor_type = method->get_class();
  }
  TRACE(TYPE, 5, "%s", SHOW(code.cfg()));
  auto local_ta = std::make_unique<local::LocalTypeAnalyzer>(
      code.cfg(), CombinedAnalyzer(clinit_type, &wps, ctor_type, nullptr));
  local_ta->run(env);

  return local_ta;
}

/*
 * The nullness analysis has an issue. That is in a method reachable from a
 * clinit or ctor in the call graph, a read of a field that is not yet
 * initialized by the 'init' method does not yield the matching nullness result
 * with the analysis. We will run into errors if we didn't handle this issue.
 *
 * The method provides a simple work around. We gather all methods reachable
 * from a clinit or ctor in the call graph. We put the reachable set into
 * any_init_reachables. In the transformation step, we do not apply null check
 * removal to methods in this set. The simple solution does not employ more
 * complex field value flow analysis, since we don't understand the value of
 * doing that at this point. But we can extend this solution at a later point.
 */
void GlobalTypeAnalysis::find_any_init_reachables(const Scope& scope,
                                                  const call_graph::Graph& cg) {
  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (!method::is_any_init(method)) {
      return;
    }
    if (!method || !method->get_code()) {
      return;
    }
    auto code = method->get_code();
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!opcode::is_an_invoke(insn->opcode())) {
        continue;
      }
      auto callee_method_def =
          resolve_method(insn->get_method(), opcode_to_search(insn), method);
      if (!callee_method_def || callee_method_def->is_external() ||
          !callee_method_def->is_concrete()) {
        continue;
      }
      if (!cg.has_node(method)) {
        TRACE(TYPE,
              5,
              "[any init reachables] missing node in cg %s",
              SHOW(method));
        continue;
      }
      auto callees = resolve_callees_in_graph(cg, method, insn);
      for (const DexMethod* callee : callees) {
        scan_any_init_reachables(cg, callee, m_any_init_reachables);
      }
    }
  });
  TRACE(TYPE, 2, "[any init reachables] size %d", m_any_init_reachables.size());
}

std::unique_ptr<GlobalTypeAnalyzer> GlobalTypeAnalysis::analyze(
    const Scope& scope) {
  call_graph::Graph cg = call_graph::single_callee_graph(scope);
  // Rebuild all CFGs here -- this should be more efficient than doing them
  // within FixpointIterator::analyze_node(), since that can get called
  // multiple times for a given method
  walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
    code.cfg().calculate_exit_block();
  });
  find_any_init_reachables(scope, cg);

  // Run the bootstrap. All field value and method return values are
  // represented by Top.
  TRACE(TYPE, 2, "[global] Bootstrap run");
  auto gta = std::make_unique<GlobalTypeAnalyzer>(cg);
  gta->run({{CURRENT_PARTITION_LABEL, ArgumentTypeEnvironment()}});
  auto non_true_virtuals = mog::get_non_true_virtuals(scope);
  size_t iteration_cnt = 0;

  for (size_t i = 0; i < m_max_global_analysis_iteration; ++i) {
    // Build an approximation of all the field values and method return values.
    TRACE(TYPE, 2, "[global] Collecting WholeProgramState");
    auto wps = std::make_unique<WholeProgramState>(
        scope, *gta, non_true_virtuals, m_any_init_reachables);
    trace_whole_program_state(*wps);
    trace_stats(*wps);
    trace_whole_program_state_diff(gta->get_whole_program_state(), *wps);
    // If this approximation is not better than the previous one, we are done.
    if (gta->get_whole_program_state().leq(*wps)) {
      break;
    }
    // Use the refined WholeProgramState to propagate more constants via
    // the stack and registers.
    TRACE(TYPE, 2, "[global] Start a new global analysis run");
    gta->set_whole_program_state(std::move(wps));
    gta->run({{CURRENT_PARTITION_LABEL, ArgumentTypeEnvironment()}});
    ++iteration_cnt;
  }

  TRACE(TYPE,
        1,
        "[global] Finished in %d global iterations (max %d)",
        iteration_cnt,
        m_max_global_analysis_iteration);
  return gta;
}

void GlobalTypeAnalysis::trace_stats(WholeProgramState& wps) {
  if (!traceEnabled(TYPE, 2)) {
    return;
  }
  TRACE(TYPE,
        2,
        "[global] wps stats: fields resolved %d; methods resolved %d",
        wps.get_num_resolved_fields(),
        wps.get_num_resolved_methods());
}

} // namespace global

} // namespace type_analyzer
