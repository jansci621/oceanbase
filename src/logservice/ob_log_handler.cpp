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

#define USING_LOG_PREFIX PALF
#include "ob_log_handler.h"
#include "ob_append_callback.h"
#include "ob_switch_leader_adapter.h"
#include "common/ob_role.h"
#include "lib/ob_define.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log.h"
#include "logservice/applyservice/ob_log_apply_service.h"
#include "logservice/replayservice/ob_log_replay_service.h"
#include "logservice/logrpc/ob_log_rpc_req.h"
#include "logservice/palf/log_define.h"
#include "logservice/palf/lsn.h"
#include "logservice/palf/palf_env.h"
#include "logservice/palf/log_group_entry.h"
#include "logservice/palf/palf_options.h"
#include "storage/tx/ob_ts_mgr.h"

namespace oceanbase
{
namespace logservice
{
using namespace palf;
ObLogHandler::ObLogHandler() : self_(),
                               apply_status_(NULL),
                               apply_service_(NULL),
                               replay_service_(NULL),
                               deps_lock_(),
                               lc_cb_(NULL),
                               rpc_proxy_(NULL),
                               append_cost_stat_("[PALF STAT APPEND COST]", 1 * 1000 * 1000),
                               cached_is_log_sync_(false),
                               last_check_sync_ts_(OB_INVALID_TIMESTAMP),
                               last_renew_loc_ts_(OB_INVALID_TIMESTAMP),
                               is_in_stop_state_(true),
                               is_inited_(false),
                               get_max_decided_log_ts_ns_debug_time_(OB_INVALID_TIMESTAMP)
{
}

ObLogHandler::~ObLogHandler()
{
  destroy();
}

int ObLogHandler::init(const int64_t id,
                       const common::ObAddr &self,
                       ObLogApplyService *apply_service,
                       ObLogReplayService *replay_service,
                       PalfHandle &palf_handle,
                       PalfEnv *palf_env,
                       PalfLocationCacheCb *lc_cb,
                       obrpc::ObLogServiceRpcProxy *rpc_proxy)
{
  int ret = OB_SUCCESS;
  ObApplyStatus *apply_status = NULL;
  ObApplyStatusGuard guard;
  share::ObLSID ls_id(id);
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (false == palf_handle.is_valid() ||
             OB_ISNULL(palf_env) ||
             OB_ISNULL(apply_service) ||
             OB_ISNULL(lc_cb) ||
             OB_ISNULL(rpc_proxy)) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid arguments", K(palf_handle), KP(palf_env), KP(lc_cb), KP(rpc_proxy));
  } else if (OB_FAIL(apply_service->get_apply_status(ls_id, guard))) {
    CLOG_LOG(WARN, "guard get apply status failed", K(ret), K(id));
  } else if (NULL == (apply_status_ = guard.get_apply_status())) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "apply status is not exist", K(ret), K(id));
  } else {
    get_max_decided_log_ts_ns_debug_time_ = OB_INVALID_TIMESTAMP;
    apply_service_ = apply_service;
    replay_service_ = replay_service;
    apply_status_->inc_ref();
    id_ = id;
    self_ = self;
    palf_handle_ = palf_handle;
    palf_env_ = palf_env;
    role_ = FOLLOWER;
    lc_cb_ = lc_cb;
    rpc_proxy_ = rpc_proxy;
    is_in_stop_state_ = false;
    is_inited_ = true;
    FLOG_INFO("ObLogHandler init success", K(id), K(palf_handle));
  }
  return ret;
}

bool ObLogHandler::is_valid() const
{
  return true == is_inited_ &&
         false == is_in_stop_state_ &&
         self_.is_valid() &&
         true == palf_handle_.is_valid() &&
         NULL != palf_env_ &&
         NULL != apply_status_ &&
         NULL != apply_service_ &&
         NULL != lc_cb_ &&
         NULL != rpc_proxy_;
}

int ObLogHandler::stop()
{
  int ret = OB_SUCCESS;
  WLockGuard guard(lock_);
  if (IS_INIT) {
    is_in_stop_state_ = true;
    //unregister_file_size_cb不能在apply status锁内, 可能会导致死锁
    apply_status_->unregister_file_size_cb();
    if (OB_FAIL(apply_status_->stop())) {
      CLOG_LOG(INFO, "apply_status stop failed", KPC(this), KPC(apply_status_), KR(ret));
    } else if (palf_handle_.is_valid()) {
      palf_env_->close(palf_handle_);
    }
    CLOG_LOG(INFO, "stop log handler finish", KPC(this), KPC(apply_status_), KR(ret));
  }
  return ret;
}

//判断is_apply_done依赖log handler不能再继续append
//所以需要is_in_stop_state_置true表示stop阶段已经不能再提交日志
int ObLogHandler::safe_to_destroy()
{
  int ret = OB_SUCCESS;
  bool is_done = false;
  LSN end_lsn;
  WLockGuard guard(lock_);
  if (IS_INIT) {
    if (palf_handle_.is_valid() || !is_in_stop_state_) {
      ret = OB_STATE_NOT_MATCH;
    } else if (OB_FAIL(apply_status_->is_apply_done(is_done, end_lsn))) {
      CLOG_LOG(ERROR, "check apply status is_apply_done failed", K(ret), K(is_done), K(end_lsn), KPC(apply_status_));
    } else if (false == is_done) {
      ret = OB_EAGAIN;
      CLOG_LOG(INFO, "wait apply done false", K(ret), K(is_done), K(end_lsn), KPC(apply_status_));
    } else {
      CLOG_LOG(INFO, "wait apply done finish", K(ret), K(is_done), K(end_lsn), KPC(apply_status_));
    }
  }
  return ret;
}

void ObLogHandler::destroy()
{
  WLockGuard guard(lock_);
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    is_inited_ = false;
    is_in_stop_state_ = true;
    common::ObSpinLockGuard deps_guard(deps_lock_);
    apply_service_->revert_apply_status(apply_status_);
    apply_status_ = NULL;
    apply_service_ = NULL;
    replay_service_ = NULL;
    if (true == palf_handle_.is_valid()) {
      palf_env_->close(palf_handle_);
    }
    lc_cb_ = NULL;
    rpc_proxy_ = NULL;
    palf_env_ = NULL;
    id_ = -1;
    get_max_decided_log_ts_ns_debug_time_ = OB_INVALID_TIMESTAMP;
  }
}

int ObLogHandler::append(const void *buffer,
                         const int64_t nbytes,
                         const int64_t ref_ts_ns,
                         const bool need_nonblock,
                         AppendCb *cb,
                         LSN &lsn,
                         int64_t &ts_ns)
{
  int ret = OB_SUCCESS;
  int64_t wait_times = 0;
  PalfAppendOptions opts;
  opts.need_nonblock = need_nonblock;
  opts.need_check_proposal_id = true;
  const int64_t begin_ts = common::ObTimeUtility::current_time();
  while (true) {
    // generate opts
    opts.proposal_id = ATOMIC_LOAD(&proposal_id_);
    do {
      RLockGuard guard(lock_);
      CriticalGuard(ls_qs_);
      cb->set_append_start_ts(ObTimeUtility::fast_current_time());
      if (IS_NOT_INIT) {
        ret = OB_NOT_INIT;
      } else if (is_in_stop_state_) {
        ret = OB_NOT_RUNNING;
      } else if (LEADER != ATOMIC_LOAD(&role_)) {
        ret = OB_NOT_MASTER;
      } else if (OB_FAIL(palf_handle_.append(opts, buffer, nbytes, ref_ts_ns, lsn, ts_ns))) {
        if (REACH_TIME_INTERVAL(1*1000*1000)) {
          CLOG_LOG(WARN, "palf_handle_ append failed", K(ret), KPC(this));
        }
      } else {
        cb->set_append_finish_ts(ObTimeUtility::fast_current_time());
        cb->__set_lsn(lsn);
        cb->__set_ts_ns(ts_ns);
        ret = apply_status_->push_append_cb(cb);
        CLOG_LOG(TRACE, "palf_handle_ push_append_cb success", K(lsn), K(ts_ns), K(ret), K(id_));
      }
    } while (0);
    // check if need wait and retry append
    if (opts.need_nonblock) {
      // nonblock mode, end loop
      break;
    } else if (OB_EAGAIN == ret) {
      // block mode, need sleep and retry for -4023 ret code
      static const int64_t MAX_SLEEP_US = 100;
      ++wait_times;
      int64_t sleep_us = wait_times * 10;
      if (sleep_us > MAX_SLEEP_US) {
        sleep_us = MAX_SLEEP_US;
      }
      ob_usleep(sleep_us);
    } else {
      // other ret code, end loop
      break;
    }
  }
  const int64_t cost_ts = common::ObTimeUtility::current_time() - begin_ts;
  append_cost_stat_.stat(cost_ts);
  return ret;
}

void ObLogHandler::switch_role(const common::ObRole &role, const int64_t proposal_id)
{
  WLockGuard guard(lock_);
  role_ = role;
  proposal_id_ = proposal_id;
}

int ObLogHandler::get_role(common::ObRole &role, int64_t &proposal_id) const
{
  int ret = OB_SUCCESS;
  bool is_pending_state = false;
  int64_t curr_palf_proposal_id;
  ObRole curr_palf_role;
  // 获取当前的proposal_id
  RLockGuard guard(lock_);
  const int64_t saved_proposal_id = ATOMIC_LOAD(&proposal_id_);
  const ObRole saved_role = ATOMIC_LOAD(&role_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (FOLLOWER == saved_role) {
    role = FOLLOWER;
    proposal_id = saved_proposal_id;
  } else if (OB_FAIL(palf_handle_.get_role(curr_palf_role, curr_palf_proposal_id, is_pending_state))) {
    CLOG_LOG(WARN, "get_role failed", K(ret), KPC(this));
  } else if (curr_palf_proposal_id != saved_proposal_id) {
    // palf的proposal_id已经发生变化，返回FOLLOWER
    role = FOLLOWER;
    proposal_id = saved_proposal_id;
  } else {
    role = curr_palf_role;
    proposal_id = saved_proposal_id;
  }
  return ret;
}

int ObLogHandler::get_access_mode(int64_t &mode_version, palf::AccessMode &access_mode) const
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (OB_FAIL(palf_handle_.get_access_mode(mode_version, access_mode))) {
    CLOG_LOG(WARN, "palf get_access_mode failed", K(ret), K_(id));
  } else {
  }
  return ret;
}

int ObLogHandler::change_access_mode(const int64_t mode_version,
                                     const palf::AccessMode &access_mode,
                                     const int64_t ref_ts_ns)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  // do not check role of LogHander with PALF, check proposal_id is enough.
  // If you want change_access_mode from RAW_WRITE to APPEND,
  // in this time, ObLogHandler is FOLLOWER and ObRestoreHandler is LEADER.
  const int64_t proposal_id = ATOMIC_LOAD(&proposal_id_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (OB_FAIL(palf_handle_.change_access_mode(proposal_id, mode_version, access_mode, ref_ts_ns))) {
    CLOG_LOG(WARN, "palf change_access_mode failed", K(ret), K_(id), K(proposal_id), K(mode_version),
        K(access_mode), K(ref_ts_ns));
  } else {
    FLOG_INFO("change_access_mode success", K(ret), K_(id), K(proposal_id), K(mode_version), K(access_mode), K(ref_ts_ns));
  }
  return ret;
}

int ObLogHandler::seek(const LSN &lsn, PalfBufferIterator &iter)
{
  RLockGuard guard(lock_);
  return palf_handle_.seek(lsn, iter);
}

int ObLogHandler::seek(const LSN &lsn, PalfGroupBufferIterator &iter)
{
  RLockGuard guard(lock_);
  return palf_handle_.seek(lsn, iter);
}

int ObLogHandler::seek(const int64_t ts_ns, palf::PalfGroupBufferIterator &iter)
{
  RLockGuard guard(lock_);
  return palf_handle_.seek(ts_ns, iter);
}

int ObLogHandler::set_initial_member_list(const common::ObMemberList &member_list,
                                          const int64_t paxos_replica_num)
{
  RLockGuard guard(lock_);
  return palf_handle_.set_initial_member_list(member_list, paxos_replica_num);
}

int ObLogHandler::set_initial_member_list(const common::ObMemberList &member_list,
                                          const common::ObMember &arb_replica,
                                          const int64_t paxos_replica_num)
{
  RLockGuard guard(lock_);
  return palf_handle_.set_initial_member_list(member_list, arb_replica, paxos_replica_num);
}

int ObLogHandler::set_election_priority(palf::election::ElectionPriority *priority)
{
  RLockGuard guard(lock_);
  return palf_handle_.set_election_priority(priority);
}

int ObLogHandler::reset_election_priority()
{
  RLockGuard guard(lock_);
  return palf_handle_.reset_election_priority();
}

int ObLogHandler::locate_by_ts_ns_coarsely(const int64_t ts_ns, LSN &result_lsn)
{
  RLockGuard guard(lock_);
  return palf_handle_.locate_by_ts_ns_coarsely(ts_ns, result_lsn);
}

int ObLogHandler::locate_by_lsn_coarsely(const LSN &lsn, int64_t &result_ts_ns)
{
  RLockGuard guard(lock_);
  return palf_handle_.locate_by_lsn_coarsely(lsn, result_ts_ns);
}

int ObLogHandler::advance_base_lsn(const LSN &lsn)
{
  RLockGuard guard(lock_);
  return palf_handle_.advance_base_lsn(lsn);
}

int ObLogHandler::get_end_lsn(LSN &lsn) const
{
  RLockGuard guard(lock_);
  return palf_handle_.get_end_lsn(lsn);
}

int ObLogHandler::get_max_lsn(LSN &lsn) const
{
  RLockGuard guard(lock_);
  return palf_handle_.get_max_lsn(lsn);
}

int ObLogHandler::get_max_ts_ns(int64_t &ts_ns) const
{
  RLockGuard guard(lock_);
  return palf_handle_.get_max_ts_ns(ts_ns);
}

int ObLogHandler::get_end_ts_ns(int64_t &ts) const
{
  RLockGuard guard(lock_);
  return palf_handle_.get_end_ts_ns(ts);
}

int ObLogHandler::get_paxos_member_list(common::ObMemberList &member_list, int64_t &paxos_replica_num) const
{
  RLockGuard guard(lock_);
  return palf_handle_.get_paxos_member_list(member_list, paxos_replica_num);
}

int ObLogHandler::get_global_learner_list(common::GlobalLearnerList &learner_list) const
{
  RLockGuard guard(lock_);
  return palf_handle_.get_global_learner_list(learner_list);
}

int ObLogHandler::enable_sync()
{
  RLockGuard guard(lock_);
  return palf_handle_.enable_sync();
}

int ObLogHandler::disable_sync()
{
  RLockGuard guard(lock_);
  return palf_handle_.disable_sync();
}

bool ObLogHandler::is_sync_enabled() const
{
  RLockGuard guard(lock_);
  return palf_handle_.is_sync_enabled();
}

int ObLogHandler::advance_base_info(const PalfBaseInfo &palf_base_info, const bool is_rebuild)
{
  int ret = OB_SUCCESS;
  bool is_replay_enabled = false;
  WLockGuard guard(lock_);
  share::ObLSID ls_id;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (FALSE_IT(ls_id = id_)) {
  } else if (OB_FAIL(replay_service_->is_enabled(ls_id, is_replay_enabled))) {
    CLOG_LOG(WARN, "check replay status failed", K(ret), K(ls_id));
  } else if (OB_UNLIKELY(is_replay_enabled)) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(WARN, "replay is not disabled", K(ret), K(ls_id));
  } else {
    if (OB_FAIL(palf_handle_.advance_base_info(palf_base_info, is_rebuild))) {
      CLOG_LOG(WARN, "advance_base_info failed", K(ret), K_(id), K(palf_base_info));
    } else {
      CLOG_LOG(INFO, "advance_base_info success", K(ret), K_(id), K(palf_base_info));
    }
  }
  return ret;
}

int ObLogHandler::get_palf_base_info(const LSN &base_lsn, PalfBaseInfo &palf_base_info)
{
  // 传入的base_lsn是ls基线数据的lsn，可能已经小于palf当前的base_lsn
  // 因此为了保证数据完整，需要以根据的base_lsn生成palf_base_info
  int ret = OB_SUCCESS;
  LSN new_base_lsn;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (false == base_lsn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(ERROR, "Invalid argument", K(ret), K(base_lsn), K(lbt()));
  } else if (FALSE_IT(new_base_lsn.val_ = lsn_2_block(base_lsn, PALF_BLOCK_SIZE) * PALF_BLOCK_SIZE)) {
  } else if (OB_FAIL(palf_handle_.get_base_info(new_base_lsn, palf_base_info))) {
    CLOG_LOG(WARN, "get_base_info failed", K(ret), K(new_base_lsn), K(base_lsn), K(palf_base_info));
  } else {
    CLOG_LOG(INFO, "get_palf_base_info success", K(ret), K(base_lsn), K(new_base_lsn), K(palf_base_info));
  }
  return ret;
}

int ObLogHandler::is_in_sync(bool &is_log_sync,
                             bool &is_need_rebuild) const
{
  int ret = OB_SUCCESS;
  is_log_sync = false;
  is_need_rebuild = false;
  LSN end_lsn;
  LSN last_rebuild_lsn;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (OB_FAIL(palf_handle_.get_end_lsn(end_lsn)) || !end_lsn.is_valid()) {
    CLOG_LOG(WARN, "get_end_lsn failed", K(ret), K_(id), K(end_lsn));
  } else if (OB_FAIL(palf_handle_.get_last_rebuild_lsn(last_rebuild_lsn))) {
    CLOG_LOG(WARN, "get_last_rebuild_lsn failed", K(ret), K_(id));
  } else if (last_rebuild_lsn.is_valid() && end_lsn < last_rebuild_lsn) {
    is_need_rebuild = true;
  } else {
    // check is log sync
  }
  int64_t local_max_ts_ns = OB_INVALID_TIMESTAMP;
  int64_t leader_max_ts_ns = OB_INVALID_TIMESTAMP;
  if (OB_SUCC(ret)) {
    static const int64_t SYNC_DELAY_TIME_THRESHOLD_NS = 3 * 1000 * 1000 * 1000L;
    const int64_t keepalive_service_interval_ns = 100 * 1000 * 1000L;  // keepalive service write log interval, 100ms
    const int64_t log_sync_threshold_ns = keepalive_service_interval_ns + SYNC_DELAY_TIME_THRESHOLD_NS;
    const int64_t SYNC_GET_LEADER_INFO_INTERVAL_US = log_sync_threshold_ns / 1000 / 2;
    bool unused_state = false;
    int64_t unused_id;
    common::ObRole role;
    if (OB_FAIL(palf_handle_.get_role(role, unused_id, unused_state))) {
      CLOG_LOG(WARN, "get_role failed", K(ret), K_(id));
    } else if (LEADER == role) {
      is_log_sync = true;
    } else if (OB_FAIL(palf_handle_.get_max_ts_ns(local_max_ts_ns)) ||
        OB_INVALID_TIMESTAMP == local_max_ts_ns) {
      CLOG_LOG(WARN, "get_max_ts_ns failed", K(ret), K_(id), K(local_max_ts_ns));
    } else if (palf_reach_time_interval(SYNC_GET_LEADER_INFO_INTERVAL_US, last_check_sync_ts_)) {
      // if reachs time interval, get max_ts_ns of leader with sync RPC
      if (OB_FAIL(get_leader_max_ts_ns_(leader_max_ts_ns))) {
        CLOG_LOG(WARN, "get_palf_max_ts_ns failed", K(ret), K_(id));
      }
    } else {
      is_log_sync = cached_is_log_sync_;
    }
    if (OB_SUCC(ret) && leader_max_ts_ns != OB_INVALID_TIMESTAMP) {
      is_log_sync = (leader_max_ts_ns - local_max_ts_ns <= log_sync_threshold_ns);
      cached_is_log_sync_ = is_log_sync;
    }
    ret = OB_SUCCESS;
  }
  CLOG_LOG(INFO, "is_in_sync", K(ret), K_(id), K(is_log_sync), K(leader_max_ts_ns), K(local_max_ts_ns),
      K_(cached_is_log_sync), K(is_need_rebuild), K(end_lsn), K(last_rebuild_lsn));
  return ret;
}

int ObLogHandler::get_leader_max_ts_ns_(int64_t &max_ts_ns) const
{
  int ret = OB_SUCCESS;
  common::ObAddr leader;
  max_ts_ns = OB_INVALID_TIMESTAMP;
  LogGetPalfStatReq req(self_, id_);
  LogGetPalfStatResp resp;
  bool need_renew_leader = false;
  if (OB_FAIL(lc_cb_->get_leader(id_, leader))) {
    CLOG_LOG(WARN, "get_leader failed", K(ret), K_(id));
    need_renew_leader = true;
  } else if (OB_FAIL(rpc_proxy_->to(leader).timeout(500 * 1000).trace_time(true). \
                     by(MTL_ID()).get_palf_stat(req, resp))) {
    CLOG_LOG(WARN, "get_palf_max_ts_ns failed", K(ret), K_(id));
    need_renew_leader = true;
  } else {
    max_ts_ns = resp.max_ts_ns_;
  }
  if (need_renew_leader && palf_reach_time_interval(500 * 1000, last_renew_loc_ts_)) {
    (void) lc_cb_->nonblock_renew_leader(id_);
  }
  return ret;
}

// @desc: change_replica_num interface
//        | 1.change_replica_num()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]--->  [leader]
//                                                              |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---     | 3. (CHANGE_REPLICA_NUM)
int ObLogHandler::change_replica_num(const common::ObMemberList &member_list,
                                     const int64_t curr_replica_num,
                                     const int64_t new_replica_num,
                                     const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!member_list.is_valid() ||
             !is_valid_replica_num(curr_replica_num) ||
             !is_valid_replica_num(new_replica_num) ||
             curr_replica_num <= new_replica_num ||
             timeout_us <= 0) {
    // NB: do not allow to change replica_num to bigger number now
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(member_list), K(curr_replica_num),
        K(new_replica_num), K(timeout_us));
  } else {
    LogConfigChangeCmd req(self_, id_, member_list, curr_replica_num, new_replica_num,
        CHANGE_REPLICA_NUM_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, "submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "change_replica_num success", KR(ret), K_(id), K(member_list),
          K(curr_replica_num), K(new_replica_num));
    }
  }
  return ret;
}

// @desc: add_member interface
//        | 1.add_member()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]--->  [leader]
//                                                              |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---     | 3. one_stage_config_change_(ADD_MEMBER)
int ObLogHandler::add_member(const common::ObMember &added_member,
                             const int64_t new_replica_num,
                             const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!added_member.is_valid() ||
             !is_valid_replica_num(new_replica_num) ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(added_member), K(new_replica_num), K(timeout_us));
  } else {
    common::ObMember dummy_member;
    LogConfigChangeCmd req(self_, id_, added_member, dummy_member, new_replica_num, ADD_MEMBER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "add_member success", KR(ret), K_(id), K(added_member), K(new_replica_num));
    }
  }
  return ret;
}

// @desc: remove_member interface
//        | 1. remove_member()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]---->  [leader]
//                                                               |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---      | 3. one_stage_config_change_(REMOVE_MEMBER)
int ObLogHandler::remove_member(const common::ObMember &removed_member,
                                const int64_t new_replica_num,
                                const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!removed_member.is_valid() ||
             !is_valid_replica_num(new_replica_num) ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(removed_member), K(new_replica_num), K(timeout_us));
  } else {
    common::ObMember dummy_member;
    LogConfigChangeCmd req(self_, id_, dummy_member, removed_member, new_replica_num, REMOVE_MEMBER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "remove_member success", KR(ret), K_(id), K(removed_member), K(new_replica_num));
    }
  }
  return ret;
}

// @desc: replace_member interface
//        | 1.replace_member()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]----->[leader]
//                                                              |
//                                                              V 3. one_stage_config_change_(ADD_MEMBER_AND_NUM)
//                                                              V 4. one_stage_config_change_(REMOVE_MEMBER_AND_NUM)
//  [any_member]  <----[5. Sync LogConfigChangeCmdResp]-----
int ObLogHandler::replace_member(const common::ObMember &added_member,
                                 const common::ObMember &removed_member,
                                 const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!added_member.is_valid() ||
             !removed_member.is_valid() ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(added_member), K(removed_member), K(timeout_us));
  } else {
    LogConfigChangeCmd req(self_, id_, added_member, removed_member, 0, REPLACE_MEMBER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "replace_member success", KR(ret), K_(id), K(added_member), K(removed_member), K(timeout_us));
    }
  }
  return ret;
}

// @desc: add_learner interface
//        | 1.add_learner()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]--->  [leader]
//                                                              |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---     | 3. one_stage_config_change_(ADD_LEARNER)
int ObLogHandler::add_learner(const common::ObMember &added_learner,
                              const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!added_learner.is_valid() ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(added_learner), K(timeout_us));
  } else {
    common::ObMember dummy_member;
    LogConfigChangeCmd req(self_, id_, added_learner, dummy_member, 0, ADD_LEARNER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "add_member success", KR(ret), K_(id), K(added_learner));
    }
  }
  return ret;
}

// @desc: remove_learner interface
//        | 1.remove_learner()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]--->  [leader]
//                                                              |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---     | 3. one_stage_config_change_(REMOVE_LEARNER)
int ObLogHandler::remove_learner(const common::ObMember &removed_learner,
                                 const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!removed_learner.is_valid() ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(removed_learner), K(timeout_us));
  } else {
    common::ObMember dummy_member;
    LogConfigChangeCmd req(self_, id_, dummy_member, removed_learner, 0, REMOVE_LEARNER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "add_member success", KR(ret), K_(id), K(removed_learner));
    }

  }
  return ret;
}

// @desc: switch_learner_to_acceptor interface
//        | 1.switch_learner_to_accetpor()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]--->  [leader]
//                                                              |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---     | 3. one_stage_config_change_(SWITCH_LEARNER_TO_ACCEPTOR)
int ObLogHandler::switch_learner_to_acceptor(const common::ObMember &learner,
                                             const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!learner.is_valid() ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(learner), K(timeout_us));
  } else {
    LogConfigChangeCmd req(self_, id_, learner, learner, 0, SWITCH_TO_ACCEPTOR_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "add_member success", KR(ret), K_(id), K(learner));
    }
  }
  return ret;
}

// @desc: switch_acceptor_to_learner interface
//        | 1.switch_acceptor_to_learner()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]--->  [leader]
//                                                              |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---     | 3. one_stage_config_change_(SWITCH_ACCEPTOR_TO_LEARNER)
int ObLogHandler::switch_acceptor_to_learner(const common::ObMember &member,
                                             const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!member.is_valid() ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(member), K(timeout_us));
  } else {
    LogConfigChangeCmd req(self_, id_, member, member, 0, SWITCH_TO_LEARNER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "add_member success", KR(ret), K_(id), K(member));
    }
  }
  return ret;
}

// @desc: add_arb_member interface
//        | 1.add_arb_member()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]--->  [leader]
//                                                              |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---     | 3. one_stage_config_change_(ADD_ARB_MEMBER)
int ObLogHandler::add_arb_member(const common::ObMember &added_member,
                                 const int64_t new_replica_num,
                                 const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!added_member.is_valid() ||
             !is_valid_replica_num(new_replica_num) ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(added_member), K(new_replica_num), K(timeout_us));
  } else {
    common::ObMember dummy_member;
    LogConfigChangeCmd req(self_, id_, added_member, dummy_member, new_replica_num, ADD_ARB_MEMBER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "add_arb_member success", KR(ret), K_(id), K(added_member), K(new_replica_num));
    }
  }
  return ret;
}

// @desc: remove_arb_member interface
//        | 1. remove_arb_member()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]---->  [leader]
//                                                               |
//  [any_member]  <----[4. Sync LogConfigChangeCmdResp]---      | 3. one_stage_config_change_(REMOVE_ARB_MEMBER)
int ObLogHandler::remove_arb_member(const common::ObMember &removed_member,
                                    const int64_t new_replica_num,
                                    const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!removed_member.is_valid() ||
             !is_valid_replica_num(new_replica_num) ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(removed_member), K(new_replica_num), K(timeout_us));
  } else {
    common::ObMember dummy_member;
    LogConfigChangeCmd req(self_, id_, dummy_member, removed_member, new_replica_num, REMOVE_ARB_MEMBER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "remove_arb_member success", KR(ret), K_(id), K(removed_member), K(new_replica_num));
    }
  }
  return ret;
}

// @desc: replace_arb_member interface
//        | 1.replace_arb_member()
//        V
//  [any_member]  -----[2. Sync LogConfigChangeCmd]----->[leader]
//                                                              |
//                                                              V 3. one_stage_config_change_(REMOVE_MEMBER_AND_NUM)
//                                                              V 4. one_stage_config_change_(ADD_MEMBER_AND_NUM)
//  [any_member]  <----[5. Sync LogConfigChangeCmdResp]-----
int ObLogHandler::replace_arb_member(const common::ObMember &added_member,
                                     const common::ObMember &removed_member,
                                     const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!added_member.is_valid() ||
             !removed_member.is_valid() ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(added_member), K(removed_member), K(timeout_us));
  } else {
    LogConfigChangeCmd req(self_, id_, added_member, removed_member, 0, REPLACE_ARB_MEMBER_CMD, timeout_us * 1000);
    if (OB_FAIL(submit_config_change_cmd_(req))) {
      CLOG_LOG(WARN, " submit_config_change_cmd failed", KR(ret), K_(id), K(req), K(timeout_us));
    } else {
      CLOG_LOG(INFO, "replace_arb_member success", KR(ret), K_(id), K(added_member), K(removed_member), K(timeout_us));
    }
  }
  return ret;
}

// @desc: degrade_acceptor_to_learner interface
//        | 1.degrade_acceptor_to_learner()
//        V
//     [leader]
int ObLogHandler::degrade_acceptor_to_learner(const common::ObMemberList &member_list,
                                              const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!member_list.is_valid() ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(member_list), K(timeout_us));
  } else if (OB_FAIL(palf_handle_.degrade_acceptor_to_learner(member_list, timeout_us * 1000L))) {
    CLOG_LOG(WARN, "degrade_acceptor_to_learner failed", KR(ret), K_(id), K(member_list), K(timeout_us));
  } else {
    CLOG_LOG(INFO, "degrade_acceptor_to_learner success", KR(ret), K_(id), K(member_list));
  }
  return ret;
}

// @desc: upgrade_learner_to_acceptor interface
//        | 1.upgrade_learner_to_acceptor()
//        V
//     [leader]
int ObLogHandler::upgrade_learner_to_acceptor(const common::ObMemberList &learner_list,
                                              const int64_t timeout_us)
{
  int ret = OB_SUCCESS;
  common::ObSpinLockGuard deps_guard(deps_lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else if (!learner_list.is_valid() ||
             timeout_us <= 0) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(learner_list), K(timeout_us));
  } else if (OB_FAIL(palf_handle_.upgrade_learner_to_acceptor(learner_list, timeout_us * 1000L))) {
    CLOG_LOG(WARN, "upgrade_learner_to_acceptor failed", KR(ret), K_(id), K(learner_list), K(timeout_us));
  } else {
    CLOG_LOG(INFO, "upgrade_learner_to_acceptor success", KR(ret), K_(id), K(learner_list));
  }
  return ret;
}

int ObLogHandler::submit_config_change_cmd_(const LogConfigChangeCmd &req)
{
  int ret = OB_SUCCESS;
  ObSwitchLeaderAdapter switch_leader_adapter;
  if (!req.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", KR(ret), K_(id), K(req));
  } else {
    constexpr int64_t MIN_CONN_TIMEOUT_US = 5 * 1000 * 1000;                // 5s
    constexpr int64_t RENEW_LEADER_INTERVAL_NS = 500 * 1000 * 1000L;        // 500ms
    const int64_t timeout_us = req.timeout_ns_ / 1000;
    const int64_t conn_timeout_us = MIN(timeout_us, MIN_CONN_TIMEOUT_US);
    const int64_t start_ts_ns = common::ObTimeUtility::current_time_ns();
    int64_t last_renew_leader_ts_ns = OB_INVALID_TIMESTAMP;
    FLOG_INFO("config_change start", K_(id), K(req));
    bool has_added_to_blacklist = false;
    bool has_removed_from_blacklist = false;
    while(OB_SUCCESS == ret || OB_NOT_MASTER == ret) {
      if (common::ObTimeUtility::current_time_ns() - start_ts_ns >= req.timeout_ns_) {
        ret = OB_TIMEOUT;
        break;
      }
      // need to remove added member from election blacklist before adding member
      if (req.is_add_member_list() && false == has_removed_from_blacklist) {
        int tmp_ret = OB_SUCCESS;
        if (OB_SUCCESS != (tmp_ret = switch_leader_adapter.remove_from_election_blacklist(id_, req.added_member_.get_server()))) {
          CLOG_LOG(WARN, "remove_from_election_blacklist failed", K(tmp_ret), K_(id), K(req));
          ob_usleep(50 * 1000);
          continue;
        } else {
          has_removed_from_blacklist = true;
        }
      }
      common::ObAddr leader;
      ConfigChangeCmdHandler cmd_handler(&palf_handle_);
      LogConfigChangeCmdResp resp;
      bool need_renew_leader = false;
      // judge init status to avoiding log_handler destoring gets stuck
      if (IS_NOT_INIT || OB_ISNULL(lc_cb_) || OB_ISNULL(rpc_proxy_)) {
        ret = OB_NOT_INIT;
        CLOG_LOG(WARN, "PalfHandleImpl not init", KR(ret), K_(id));
      } else if (OB_FAIL(lc_cb_->get_leader(id_, leader))) {
        need_renew_leader = true;
        ret = OB_SUCCESS;
      } else if (leader == self_ && FALSE_IT(resp.ret_ = cmd_handler.handle_config_change_cmd(req))) {
      } else if (leader != self_  && OB_FAIL(rpc_proxy_->to(leader).timeout(conn_timeout_us).trace_time(true).
                         max_process_handler_time(timeout_us).by(MTL_ID()).send_log_config_change_cmd(req, resp))) {
        // if RPC fails, try again
        ret = OB_SUCCESS;
        if (common::is_server_down_error(ret)) {
          need_renew_leader = true;
        }
      } else if (OB_SUCC(resp.ret_)) {
        FLOG_INFO("config_change finish", KR(ret), KPC(this), K(req),
            "cost time(ns)", common::ObTimeUtility::current_time_ns() - start_ts_ns);
        break;
      } else if (OB_EAGAIN == ret) {
        ret = OB_SUCCESS;
        ob_usleep(50 * 1000);
      } else if (OB_NOT_MASTER == ret) {
        need_renew_leader = true;
      } else if (OB_NOT_ALLOW_REMOVING_LEADER == ret &&
                 req.is_remove_member_list() &&
                 (req.removed_member_.get_server() == leader)) {
        ret = OB_SUCCESS;
        int tmp_ret = OB_SUCCESS;
        // if removed_member is leader, switch leadership to another node and try again
        // if meta tenant's leader is down, add_to_election_blacklist may return fail,
        // need to retry add_to_election_blacklist until timeout/success
        if (true == has_added_to_blacklist ||
            OB_SUCCESS != (tmp_ret = switch_leader_adapter.add_to_election_blacklist(id_, leader))) {
          if (tmp_ret != OB_SUCCESS && REACH_TIME_INTERVAL(1 * 1000 * 1000)) {
            CLOG_LOG(WARN, "add_to_election_blacklist failed", KR(tmp_ret), K_(id), K_(self));
          }
        } else {
          has_added_to_blacklist = true;
          need_renew_leader = true;
        }
      } else {
        CLOG_LOG(WARN, "handle_config_change_cmd failed", KR(ret), KPC(this), K(req), K(leader));
      }
      if (need_renew_leader &&
          common::ObTimeUtility::current_time_ns() - last_renew_leader_ts_ns > RENEW_LEADER_INTERVAL_NS) {
        last_renew_leader_ts_ns = common::ObTimeUtility::current_time_ns();
        ret = lc_cb_->nonblock_renew_leader(id_);
        CLOG_LOG(INFO, "renew location cache leader", KR(ret), K_(id));
      }
    }
  }
  return ret;
}

int ObLogHandler::is_valid_member(const common::ObAddr &addr,
                                  bool &is_valid) const
{
  int ret = OB_SUCCESS;
  common::ObRole role;
  common::ObRole new_role;
  int64_t proposal_id;
  int64_t new_proposal_id;
  common::ObMemberList member_list;
  int64_t paxos_replica_num = 0;
  bool is_pending_state = false;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    CLOG_LOG(WARN, "loghandler not inited or maybe destroyed", K(ret), K(addr));
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
    CLOG_LOG(INFO, "loghandler is stopped", K(ret), K(addr));
  } else if (!addr.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(ERROR, "invalid arguments", K(ret), K(addr), K(id_));
  } else if (OB_FAIL(palf_handle_.get_role(role, proposal_id, is_pending_state))) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(ERROR, "get_role failed", K(ret), KPC(this));
  } else if (LEADER != role) {
    ret = OB_NOT_MASTER;
  } else if (OB_FAIL(palf_handle_.get_paxos_member_list(member_list, paxos_replica_num))) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(ERROR, "get_paxos_member_list failed", K(ret), KPC(this));
  } else if (OB_FAIL(palf_handle_.get_role(new_role, new_proposal_id, is_pending_state))) {
    ret = OB_ERR_UNEXPECTED;
    CLOG_LOG(ERROR, "get_role failed", K(ret), KPC(this));
  } else {
    if (role == new_role && proposal_id == new_proposal_id) {
      is_valid = member_list.contains(addr);
    } else {
      ret = OB_NOT_MASTER;
      CLOG_LOG(INFO, "role changed during is_valid_member", K(ret), KPC(this), K(role),
               K(new_role), K(proposal_id), K(new_proposal_id));
    }
  }
  return ret;
}

void ObLogHandler::wait_append_sync() {
  WaitQuiescent(ls_qs_);
}

int ObLogHandler::enable_replay(const palf::LSN &lsn,
                                const int64_t &log_ts)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  share::ObLSID id;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (FALSE_IT(id = id_)) {
  } else if (!lsn.is_valid() || OB_INVALID_TIMESTAMP == log_ts) {
    ret = OB_INVALID_ARGUMENT;
    CLOG_LOG(WARN, "invalid argument", K(ret), K(id), K(lsn), K(log_ts));
  } else if (OB_FAIL(replay_service_->enable(id, lsn, log_ts))) {
    CLOG_LOG(WARN, "failed to enable replay", K(ret), K(id), K(lsn), K(log_ts));
  } else {
    CLOG_LOG(INFO, "enable replay success", K(ret), K(id), K(lsn), K(log_ts));
  }
  return ret;
}

int ObLogHandler::disable_replay()
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  share::ObLSID id;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (FALSE_IT(id = id_)) {
  } else if (OB_FAIL(replay_service_->disable(id))) {
    CLOG_LOG(WARN, "failed to disable replay", K(ret), K(id));
  } else {
    CLOG_LOG(INFO, "disable replay success", K(ret), K(id));
  }
  return ret;
}

int ObLogHandler::pend_submit_replay_log()
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  share::ObLSID id;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (FALSE_IT(id = id_)) {
  } else if (OB_FAIL(replay_service_->set_submit_log_pending(id))) {
    CLOG_LOG(WARN, "failed to set_submit_log_pending", K(ret), K(id));
  } else {
    CLOG_LOG(INFO, "set_submit_log_pending success", K(ret), K(id));
  }
  return ret;
}

int ObLogHandler::restore_submit_replay_log()
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  share::ObLSID id;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (FALSE_IT(id = id_)) {
  } else if (OB_FAIL(replay_service_->erase_submit_log_pending(id))) {
    CLOG_LOG(WARN, "failed to erase_submit_log_pending", K(ret), K(id));
  } else {
    CLOG_LOG(INFO, "erase_submit_log_pending success", K(ret), K(id));
  }
  return ret;
}

bool ObLogHandler::is_replay_enabled() const
{
  bool bool_ret = false;
  int tmp_ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  share::ObLSID id;
  if (IS_NOT_INIT) {
  } else if (FALSE_IT(id = id_)) {
  } else if (OB_SUCCESS != (tmp_ret = replay_service_->is_enabled(id, bool_ret))) {
    CLOG_LOG(WARN, "check replay service is enabled failed", K(tmp_ret), K(id));
  } else {
    // do nothing
  }
  return bool_ret;
}

int ObLogHandler::get_max_decided_log_ts_ns(int64_t &log_ts)
{
  int ret = OB_SUCCESS;
  int64_t min_unreplay_log_ts_ns = OB_INVALID_TIMESTAMP;
  int64_t min_unapply_log_ts_ns = OB_INVALID_TIMESTAMP;
  share::ObLSID id;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    //和replay service统一返回4109
    ret = OB_STATE_NOT_MATCH;
  } else if (FALSE_IT(id = id_)) {
  } else if (OB_FAIL(apply_service_->get_min_unapplied_log_ts_ns(id, min_unapply_log_ts_ns))) {
    CLOG_LOG(WARN, "failed to get_min_unapplied_log_ts_ns", K(ret), K(id));
  } else if (OB_FAIL(replay_service_->get_min_unreplayed_log_ts_ns(id, min_unreplay_log_ts_ns))) {
    if (OB_STATE_NOT_MATCH != ret) {
      CLOG_LOG(WARN, "failed to get_min_unreplayed_log_ts_ns", K(ret), K(id));
    } else if (palf_reach_time_interval(1000 * 1000, get_max_decided_log_ts_ns_debug_time_)) {
      CLOG_LOG(WARN, "failed to get_min_unreplayed_log_ts_ns, replay status is not enabled", K(ret), K(id));
    }
    if (OB_STATE_NOT_MATCH == ret && OB_INVALID_TIMESTAMP != min_unapply_log_ts_ns) {
      //回放尚未enable,但是apply service中拿到的最大连续回调位点合法
      ret = OB_SUCCESS;
      log_ts = min_unapply_log_ts_ns - 1 > 0 ? min_unapply_log_ts_ns - 1 : 0;
      if (palf_reach_time_interval(1000 * 1000, get_max_decided_log_ts_ns_debug_time_)) {
        CLOG_LOG(INFO, "replay is not enabled, get_max_decided_log_ts_ns from apply", K(ret), K(id),
                 K(min_unreplay_log_ts_ns), K(min_unapply_log_ts_ns), K(log_ts));
      }
    }
  } else {
    log_ts = std::max(min_unreplay_log_ts_ns - 1, min_unapply_log_ts_ns - 1) > 0 ?
             std::max(min_unreplay_log_ts_ns - 1, min_unapply_log_ts_ns - 1) : 0;
    CLOG_LOG(TRACE, "get_max_decided_log_ts_ns", K(ret), K(id), K(min_unreplay_log_ts_ns), K(min_unapply_log_ts_ns), K(log_ts));
  }
  return ret;
}

int ObLogHandler::set_region(const common::ObRegion &region)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else {
    ret = palf_handle_.set_region(region);
  }
  return ret;
}

int ObLogHandler::disable_vote()
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else {
    ret = palf_handle_.disable_vote();
  }
  return ret;
}

int ObLogHandler::enable_vote()
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else {
    ret = palf_handle_.enable_vote();
  }
  return ret;
}

int ObLogHandler::register_rebuild_cb(palf::PalfRebuildCb *rebuild_cb)
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else {
    ret = palf_handle_.register_rebuild_cb(rebuild_cb);
  }
	return ret;
}

int ObLogHandler::unregister_rebuild_cb()
{
  int ret = OB_SUCCESS;
  RLockGuard guard(lock_);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (is_in_stop_state_) {
    ret = OB_NOT_RUNNING;
  } else {
    ret = palf_handle_.unregister_rebuild_cb();
  }
	return ret;
}

} // end namespace logservice
} // end napespace oceanbase
