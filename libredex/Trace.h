/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "Util.h"

#define TMS              \
  TM(ADD_REDEX_TXT)      \
  TM(ACCESS)             \
  TM(ANNO)               \
  TM(ARGS)               \
  TM(BBPROFILE)          \
  TM(BIND)               \
  TM(BRIDGE)             \
  TM(BUILDERS)           \
  TM(CFG)                \
  TM(CFP)                \
  TM(CLP_GQL)            \
  TM(CLP_LITHO)          \
  TM(CONSTP)             \
  TM(CPG)                \
  TM(CUSTOMSORT)         \
  TM(DBGSTRIP)           \
  TM(DC)                 \
  TM(DCE)                \
  TM(DEAD_CODE)          \
  TM(DEDUP_BLOCKS)       \
  TM(DEDUP_RES)          \
  TM(DELINIT)            \
  TM(DELMET)             \
  TM(DRAC)               \
  TM(EMPTY)              \
  TM(FINALINLINE)        \
  TM(HOTNESS)            \
  TM(ICONSTP)            \
  TM(IDEX)               \
  TM(GETTER)             \
  TM(INL)                \
  TM(INLINIT)            \
  TM(INLRES)             \
  TM(INSTRUMENT)         \
  TM(INTF)               \
  TM(LITHO_BLD)          \
  TM(LOC)                \
  TM(MAGIC_FIELDS)       \
  TM(MAIN)               \
  TM(MMINL)              \
  TM(MMODE)              \
  TM(MORTIROLO)          \
  TM(MTRANS)             \
  TM(OBFUSCATE)          \
  TM(OPTRES)             \
  TM(ORIGINALNAME)       \
  TM(OUTLINE)            \
  TM(PEEPHOLE)           \
  TM(PGR)                \
  TM(PM)                 \
  TM(PTA)                \
  TM(QUICK)              \
  TM(REDEX)              \
  TM(REACH)              \
  TM(REACH_DUMP)         \
  TM(REFU)               \
  TM(REG)                \
  TM(RELO)               \
  TM(RENAME)             \
  TM(RME)                \
  TM(RMGOTO)             \
  TM(RMU)                \
  TM(RMUF)               \
  TM(RM_INTF)            \
  TM(SDIS)               \
  TM(SHORTEN)            \
  TM(SINK)               \
  TM(SINL)               \
  TM(SPLIT_RES)          \
  TM(STR_SIMPLE)         \
  TM(SUPER)              \
  TM(SYNT)               \
  TM(TIME)               \
  TM(TRACKRESOURCES)     \
  TM(UNTF)               \
  TM(VERIFY)             \
  TM(ANALYSIS_REF_GRAPH) \
  TM(VIRT)               \
  TM(TERA)               \
  TM(BRCR)               \
  TM(SWIN)               \
  TM(SWCL)               \
  TM(SW)                 \
  TM(IFCS_ANALYSIS)      \
  TM(UNREF_INTF)

enum TraceModule : int {
#define TM(x) x,
  TMS
#undef TM
      N_TRACE_MODULES,
};

bool traceEnabled(TraceModule module, int level);
#ifdef NDEBUG
#define TRACE(...)
#else
void trace(TraceModule module, int level, const char* fmt, ...);
#define TRACE(module, level, fmt, ...)          \
  do {                                          \
    if (traceEnabled(module, level)) {          \
      trace(module, level, fmt, ##__VA_ARGS__); \
    }                                           \
  } while (0)
#endif // NDEBUG

struct TraceContext {
  explicit TraceContext(const std::string& current_method) {
    s_current_method = &current_method;
  }
  ~TraceContext() { s_current_method = nullptr; }

  thread_local static const std::string* s_current_method;
  static std::mutex s_trace_mutex;
};
