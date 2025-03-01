/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef LOGSERVICE_PALF_ELECTION_UTILS_OB_ELECTION_COMMON_DEFINE_H
#define LOGSERVICE_PALF_ELECTION_UTILS_OB_ELECTION_COMMON_DEFINE_H

// this file should only include in .cpp file,
// or may cause MACRO pollution
#include "lib/oblog/ob_log_module.h"
#include "share/ob_occam_time_guard.h"

#define LOG_PHASE(level, phase, info, args...) \
do {\
  if (phase == LogPhase::NONE) {\
    ELECT_LOG(level, info, ##args, PRINT_WRAPPER);\
  } else {\
    char joined_info[256] = {0};\
    int64_t pos = 0;\
    switch (phase) {\
    case LogPhase::INIT:\
      oceanbase::common::databuff_printf(joined_info, 256, pos, "[INIT]%s", info);\
      break;\
    case LogPhase::DESTROY:\
      oceanbase::common::databuff_printf(joined_info, 256, pos, "[DESTROY]%s", info);\
      break;\
    case LogPhase::ELECT_LEADER:\
      oceanbase::common::databuff_printf(joined_info, 256, pos, "[ELECT_LEADER]%s", info);\
      break;\
    case LogPhase::RENEW_LEASE:\
      oceanbase::common::databuff_printf(joined_info, 256, pos, "[RENEW_LEASE]%s", info);\
      break;\
    case LogPhase::CHANGE_LEADER:\
      oceanbase::common::databuff_printf(joined_info, 256, pos, "[CHANGE_LEADER]%s", info);\
      break;\
    case LogPhase::EVENT:\
      oceanbase::common::databuff_printf(joined_info, 256, pos, "[EVENT]%s", info);\
      break;\
    case LogPhase::SET_MEMBER:\
      oceanbase::common::databuff_printf(joined_info, 256, pos, "[SET_MEMBER]%s", info);\
      break;\
    default:\
      oceanbase::common::databuff_printf(joined_info, 256, pos, "[UNKNOWN]%s", info);\
      break;\
    }\
    ELECT_LOG(level, joined_info, ##args, PRINT_WRAPPER);\
  }\
} while(0)
#define LOG_INIT(level, info, args...) LOG_PHASE(level, LogPhase::INIT, info, ##args)
#define LOG_DESTROY(level, info, args...) LOG_PHASE(level, LogPhase::DESTROY, info, ##args)
#define LOG_ELECT_LEADER(level, info, args...) LOG_PHASE(level, LogPhase::ELECT_LEADER, info, ##args)
#define LOG_RENEW_LEASE(level, info, args...) LOG_PHASE(level, LogPhase::RENEW_LEASE, info, ##args)
#define LOG_CHANGE_LEADER(level, info, args...) LOG_PHASE(level, LogPhase::CHANGE_LEADER, info, ##args)
#define LOG_EVENT(level, info, args...) LOG_PHASE(level, LogPhase::EVENT, info, ##args)
#define LOG_SET_MEMBER(level, info, args...) LOG_PHASE(level, LogPhase::SET_MEMBER, info, ##args)
#define LOG_NONE(level, info, args...) LOG_PHASE(level, LogPhase::NONE, info, ##args)
#define ELECT_TIME_GUARD(func_cost_threshold) TIMEGUARD_INIT(ELECT, func_cost_threshold, 10_s)

namespace oceanbase
{
namespace palf
{
namespace election
{

enum class LogPhase
{
  NONE = 0,
  INIT = 1,
  DESTROY = 2,
  ELECT_LEADER = 3,
  RENEW_LEASE = 4,
  CHANGE_LEADER = 5,
  EVENT = 6,
  SET_MEMBER = 7,
};

constexpr int64_t MSG_DELAY_WARN_THRESTHOLD = 200_ms;
constexpr int64_t MAX_LEASE_TIME = 10_s;
constexpr int64_t PRIORITY_BUFFER_SIZE = 512;
constexpr int64_t INVALID_VALUE = -1;// 所有int64_t变量的初始默认无效值
extern int64_t MAX_TST; // 最大单程消息延迟，暂设为750ms，在单测中会将其调低，日后可改为配置项，现阶段先用全局变量代替
inline int64_t CALCULATE_RENEW_LEASE_INTERVAL() { return  0.5 * MAX_TST; }// 续约的周期，目前是325ms，在暂时没有切主流程优化的情况下，设置的间隔短一些，为了及时切主
inline int64_t CALCULATE_TIME_WINDOW_SPAN_TS() { return  2 * MAX_TST; }// 时间窗口的长度，为两个最大单程消息延迟
inline int64_t CALCULATE_MAX_ELECT_COST_TIME() { return  10 * MAX_TST; }// 一次选举可能出现的最大耗时设置，设置为10s
inline int64_t CALCULATE_LEASE_INTERVAL() { return 4 * MAX_TST; }// 4个消息延迟是3s

}// namespace election
}// namespace palf
}// namesapce oceanbase

#endif