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

#define USING_LOG_PREFIX STORAGE

#include "lib/utility/utility.h"
#include "lib/oblog/ob_log.h"
#include "observer/ob_srv_network_frame.h"
#include "observer/omt/ob_tenant_config_mgr.h"  // ObTenantConfigGuard
#include "rootserver/freeze/ob_major_freeze_helper.h"
#include "share/allocator/ob_memstore_allocator_mgr.h"
#include "share/config/ob_server_config.h"
#include "share/rc/ob_tenant_module_init_ctx.h"
#include "storage/ls/ob_ls.h"
#include "storage/tx_storage/ob_ls_handle.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/tx_storage/ob_tenant_freezer.h"

namespace oceanbase
{
namespace storage
{

typedef ObMemstoreAllocatorMgr::TAllocator ObTenantMemstoreAllocator;

ObTenantFreezer::ObTenantFreezer()
	: is_inited_(false),
    is_freezing_tx_data_(false),
    svr_rpc_proxy_(nullptr),
    common_rpc_proxy_(nullptr),
    rs_mgr_(nullptr),
    config_(nullptr),
    allocator_mgr_(nullptr),
    exist_ls_freezing_(false),
    last_update_ts_(0)
{}

ObTenantFreezer::~ObTenantFreezer()
{
	destroy();
}

void ObTenantFreezer::destroy()
{
  is_freezing_tx_data_ = false;
  exist_ls_freezing_ = false;
  self_.reset();
  svr_rpc_proxy_ = nullptr;
  common_rpc_proxy_ = nullptr;
  rs_mgr_ = nullptr;
  config_ = nullptr;
  allocator_mgr_ = nullptr;

  is_inited_ = false;
}

int ObTenantFreezer::mtl_init(ObTenantFreezer* &m)
{
  return m->init();
}

int ObTenantFreezer::init()
{
	int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("[TenantFreezer] tenant freezer init twice.", KR(ret));
  } else if (OB_UNLIKELY(!GCONF.self_addr_.is_valid()) ||
             OB_ISNULL(GCTX.net_frame_) ||
             OB_ISNULL(GCTX.srv_rpc_proxy_) ||
             OB_ISNULL(GCTX.rs_rpc_proxy_) ||
             OB_ISNULL(GCTX.rs_mgr_) ||
             OB_ISNULL(GCTX.config_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("[TenantFreezer] invalid argument", KR(ret), KP(GCTX.srv_rpc_proxy_),
             KP(GCTX.rs_rpc_proxy_), KP(GCTX.rs_mgr_), KP(GCTX.config_),
             K(GCONF.self_addr_));
  } else if (OB_FAIL(freeze_trigger_pool_.init_and_start(FREEZE_TRIGGER_THREAD_NUM))) {
    LOG_WARN("[TenantFreezer] fail to initialize freeze trigger pool", KR(ret));
  } else if (OB_FAIL(freeze_trigger_timer_.init_and_start(freeze_trigger_pool_,
                                                          TIME_WHEEL_PRECISION,
                                                          "FrzTrigger"))) {
    LOG_WARN("[TenantFreezer] fail to initialize freeze trigger timer", K(ret));
  } else if (OB_FAIL(rpc_proxy_.init(GCTX.net_frame_->get_req_transport(),
                                     GCONF.self_addr_))) {
    LOG_WARN("[TenantFreezer] fail to init rpc proxy", KR(ret));
  } else {
    is_freezing_tx_data_ = false;
    self_ = GCONF.self_addr_;
    svr_rpc_proxy_ = GCTX.srv_rpc_proxy_;
    common_rpc_proxy_ = GCTX.rs_rpc_proxy_;
    rs_mgr_ = GCTX.rs_mgr_;
    config_ = GCTX.config_;
    allocator_mgr_ = &ObMemstoreAllocatorMgr::get_instance();
    tenant_info_.tenant_id_ = MTL_ID();
    is_inited_ = true;
  }
  return ret;
}

int ObTenantFreezer::start()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant freezer not inited", KR(ret));
  } else if (OB_FAIL(freeze_trigger_timer_.
      schedule_task_repeat(timer_handle_,
                           FREEZE_TRIGGER_INTERVAL,
                           [this]() {
                             LOG_INFO("====== tenant freeze timer task ======");
                             this->check_and_do_freeze();
                             return false; // TODO: false means keep running, true means won't run again
                           }))) {
    LOG_WARN("[TenantFreezer] freezer trigger timer start failed", KR(ret));
  } else {
    LOG_INFO("[TenantFreezer] ObTenantFreezer start", K_(tenant_info));
  }
  return ret;
}

int ObTenantFreezer::stop()
{
	int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant freezer not inited", KR(ret));
  } else {
    timer_handle_.stop(); // stop freeze_trigger_timer_;
    // task_list_.stop_all();
    LOG_INFO("[TenantFreezer] ObTenantFreezer stoped done", K(timer_handle_), K_(tenant_info));
  }
  return ret;
}

void ObTenantFreezer::wait()
{
  timer_handle_.wait();
  // task_list_.wait_all();
  LOG_INFO("[TenantFreezer] ObTenantFreezer wait done", K(timer_handle_), K_(tenant_info));
}

bool ObTenantFreezer::exist_ls_freezing()
{
  int64_t cur_ts = ObTimeUtility::fast_current_time();
  int64_t old_ts = last_update_ts_;

  if ((cur_ts - last_update_ts_ > UPDATE_INTERVAL) &&
      old_ts == ATOMIC_CAS(&last_update_ts_, old_ts, cur_ts)) {
    int ret = OB_SUCCESS;
    common::ObSharedGuard<ObLSIterator> iter;
    ObLSService *ls_srv = MTL(ObLSService *);
    if (IS_NOT_INIT) {
      ret = OB_NOT_INIT;
      LOG_WARN("[TenantFreezer] tenant freezer not inited", KR(ret));
    } else if (OB_FAIL(ls_srv->get_ls_iter(iter, ObLSGetMod::TXSTORAGE_MOD))) {
      LOG_WARN("[TenantFreezer] fail to get log stream iterator", KR(ret));
    } else {
      ObLS *ls = nullptr;
      int ls_cnt = 0;
      int exist_ls_freezing = false;
      for (; OB_SUCC(iter->get_next(ls)); ++ls_cnt) {
        if (ls->get_freezer()->is_freeze()) {
          exist_ls_freezing = true;
        }
      }
      exist_ls_freezing_ = exist_ls_freezing;

      if (ret == OB_ITER_END) {
        ret = OB_SUCCESS;
      } else {
         LOG_WARN("[TenantFreezer] iter ls failed", K(ret));
      }
    }
  }

  return exist_ls_freezing_;
}

int ObTenantFreezer::ls_freeze_(ObLS *ls)
{
  int ret = OB_SUCCESS;
  const int64_t SLEEP_TS = 1000 * 1000; // 1s
  int64_t retry_times = 0;
  // wait if there is a freeze is doing
  do {
    retry_times++;
    if (OB_FAIL(ls->logstream_freeze()) && OB_ENTRY_EXIST == ret) {
      ob_usleep(SLEEP_TS);
    }
    if (retry_times % 10 == 0) {
      LOG_WARN("wait ls freeze finished cost too much time", K(retry_times));
    }
  } while (ret == OB_ENTRY_EXIST);
  return ret;
}

int ObTenantFreezer::tenant_freeze()
{
  int ret = OB_SUCCESS;
  int first_fail_ret = OB_SUCCESS;
  common::ObSharedGuard<ObLSIterator> iter;
  ObLSService *ls_srv = MTL(ObLSService *);
  FLOG_INFO("[TenantFreezer] tenant_freeze start", KR(ret));

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant freezer not inited", KR(ret));
  } else if (OB_FAIL(ls_srv->get_ls_iter(iter, ObLSGetMod::TXSTORAGE_MOD))) {
    LOG_WARN("[TenantFreezer] fail to get log stream iterator", KR(ret));
  } else {
    ObLS *ls = nullptr;
    int ls_cnt = 0;
    for (; OB_SUCC(iter->get_next(ls)); ++ls_cnt) {
      // wait until this ls freeze finished to make sure not freeze frequently because
      // of this ls freeze stuck.
      if (OB_FAIL(ls_freeze_(ls))) {
        if (OB_SUCCESS == first_fail_ret) {
          first_fail_ret = ret;
        }
        LOG_WARN("[TenantFreezer] fail to freeze logstream", KR(ret), K(ls->get_ls_id()));
      }
    }
    if (ret == OB_ITER_END) {
      ret = OB_SUCCESS;
      if (ls_cnt > 0) {
        LOG_INFO("[TenantFreezer] succeed to freeze tenant", KR(ret), K(ls_cnt));
      } else {
        LOG_WARN("[TenantFreezer] no logstream", KR(ret), K(ls_cnt));
      }
    }
    if (first_fail_ret != OB_SUCCESS &&
        first_fail_ret != OB_ITER_END) {
      ret = first_fail_ret;
    }
  }

  return ret;
}

int ObTenantFreezer::tablet_freeze(const common::ObTabletID &tablet_id,
                                   const bool is_force_freeze)
{
  int ret = OB_SUCCESS;
  share::ObLSID ls_id;
  bool is_cache_hit = false;
  ObLSService *ls_srv = MTL(ObLSService *);
  ObLSHandle handle;
  ObLS *ls = nullptr;
  FLOG_INFO("[TenantFreezer] tablet_freeze start", KR(ret), K(tablet_id));

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant freezer not inited", KR(ret));
  } else if (OB_UNLIKELY(nullptr == GCTX.location_service_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("[TenantFreezer] location service ptr is null", KR(ret));
  } else if (OB_FAIL(GCTX.location_service_->get(tenant_info_.tenant_id_,
                                                 tablet_id,
                                                 INT64_MAX,
                                                 is_cache_hit,
                                                 ls_id))) {
    LOG_WARN("[TenantFreezer] fail to get ls id according to tablet_id", KR(ret), K(tablet_id));
  } else if (OB_FAIL(ls_srv->get_ls(ls_id, handle, ObLSGetMod::TXSTORAGE_MOD))) {
    LOG_WARN("[TenantFreezer] fail to get ls", K(ret), K(ls_id));
  } else if (OB_ISNULL(ls = handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[TenantFreezer] ls is null", KR(ret), K(ls_id));
  } else if (OB_FAIL(is_force_freeze
                     ? ls->force_tablet_freeze(tablet_id)
                     : ls->tablet_freeze(tablet_id))) {
    LOG_WARN("[TenantFreezer] fail to freeze tablet", KR(ret), K(ls_id), K(tablet_id));
  } else {
    LOG_INFO("[TenantFreezer] succeed to freeze tablet", KR(ret), K(ls_id), K(tablet_id));
  }

  return ret;
}

int ObTenantFreezer::get_ls_tx_data_mem_used_(ObLS *ls, int64_t &ls_tx_data_mem_used)
{
  int ret = OB_SUCCESS;
  ObMemtableMgrHandle mgr_handle;
  ObTxDataMemtableMgr *memtable_mgr = nullptr;
  ObTableHandleV2 memtable_handle;
  ObTxDataMemtable *memtable = nullptr;
  if (OB_ISNULL(ls)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[TenantFreezer] get ls tx data mem used failed.", KR(ret));
  } else if (OB_FAIL(ls->get_tablet_svr()->get_tx_data_memtable_mgr(mgr_handle))) {
    LOG_WARN("[TenantFreezer] get tx data memtable mgr failed.", KR(ret));
  } else if (OB_ISNULL(memtable_mgr
                       = static_cast<ObTxDataMemtableMgr *>(mgr_handle.get_memtable_mgr()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[TenantFreezer] tx data memtable mgr is unexpected nullptr.", KR(ret));
  } else if (OB_FAIL(memtable_mgr->get_active_memtable(memtable_handle))) {
    LOG_WARN("get active memtable from tx data memtable mgr failed.", KR(ret));
  } else if (OB_FAIL(memtable_handle.get_tx_data_memtable(memtable))) {
    STORAGE_LOG(ERROR, "get tx data memtable failed.", KR(ret), K(tenant_info_.tenant_id_));
  } else if (OB_ISNULL(memtable)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(ERROR, "unexpected nullptr of tx data memtable", KR(ret), K(tenant_info_.tenant_id_));
  } else {
    ls_tx_data_mem_used = memtable->get_occupied_size();
  }
  return ret;
}

int ObTenantFreezer::get_tenant_tx_data_mem_used_(int64_t &tenant_tx_data_mem_used)
{
  int ret = OB_SUCCESS;
  common::ObSharedGuard<ObLSIterator> iter;
  ObLSService *ls_srv = MTL(ObLSService *);

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant freezer not inited", KR(ret));
  } else if (OB_FAIL(ls_srv->get_ls_iter(iter, ObLSGetMod::TXSTORAGE_MOD))) {
    LOG_WARN("[TenantFreezer] fail to get log stream iterator", KR(ret));
  } else {
    ObLS *ls = nullptr;
    int ls_cnt = 0;
    int64_t ls_tx_data_mem_used = 0;
    for (; OB_SUCC(ret) && OB_SUCC(iter->get_next(ls)); ++ls_cnt) {
      if (OB_FAIL(get_ls_tx_data_mem_used_(ls, ls_tx_data_mem_used))) {
        LOG_WARN("[TenantFreezer] fail to get tx data mem used in one ls", KR(ret));
      } else {
        tenant_tx_data_mem_used += ls_tx_data_mem_used;
      }
    }

    if (ret == OB_ITER_END) {
      ret = OB_SUCCESS;
      if (0 == ls_cnt) {
        LOG_WARN("[TenantFreezer] no logstream", KR(ret), K(ls_cnt), K(tenant_info_));
      }
    }
  }

  return ret;
}

int ObTenantFreezer::check_and_freeze_normal_data_()
{

  int ret = OB_SUCCESS;
  bool upgrade_mode = GCONF.in_major_version_upgrade_mode();
  int tmp_ret = OB_SUCCESS;
  bool need_freeze = false;
  int64_t active_memstore_used = 0;
  int64_t total_memstore_used = 0;
  int64_t total_memstore_hold = 0;
  int64_t memstore_freeze_trigger = 0;
  if (OB_UNLIKELY(upgrade_mode)) {
    // skip trigger freeze while upgrading
  } else {
    {
      SpinRLockGuard guard(lock_);
      if (!tenant_info_.is_loaded_) {
        // do nothing
      } else if (OB_FAIL(get_freeze_trigger_(memstore_freeze_trigger))) {
        LOG_WARN("[TenantFreezer] fail to get minor freeze trigger", KR(ret));
      } else if (OB_FAIL(get_tenant_mem_usage_(active_memstore_used,
                                               total_memstore_used,
                                               total_memstore_hold))) {
        LOG_WARN("[TenantFreezer] fail to get mem usage", KR(ret));
      } else {
        need_freeze = need_freeze_(active_memstore_used,
                                   memstore_freeze_trigger);
        if (need_freeze && !is_minor_need_slow_(total_memstore_hold, memstore_freeze_trigger)) {
          unset_tenant_slow_freeze_();
        }
        log_frozen_memstore_info_if_need_(active_memstore_used, total_memstore_used,
                                          total_memstore_hold, memstore_freeze_trigger);
        halt_prewarm_if_need_(memstore_freeze_trigger, total_memstore_hold);
      }
    }
    // must out of the lock, to make sure there is no deadlock, just because of tenant freeze hung.
    if (OB_TMP_FAIL(do_major_if_need_(need_freeze))) {
      LOG_WARN("[TenantFreezer] fail to do major freeze", K(tmp_ret));
    }
    if (need_freeze) {
      if (OB_TMP_FAIL(do_minor_freeze_(active_memstore_used,
                                       memstore_freeze_trigger))) {
        LOG_WARN("[TenantFreezer] fail to do minor freeze", K(tmp_ret));
      }
    }
  }
  return ret;
}

int ObTenantFreezer::check_and_freeze_tx_data_()
{
  int ret = OB_SUCCESS;
  int64_t tenant_tx_data_mem_used = 0;

  static int skip_count = 0;
  if (true == ATOMIC_LOAD(&is_freezing_tx_data_)) {
    // skip freeze when there is another self freeze task is running
    if (++skip_count > 10) {
      int64_t cost_time = (FREEZE_TRIGGER_INTERVAL * skip_count);
      LOG_WARN("A tx data tenant self freeze task cost too much time",
                  K(tenant_info_.tenant_id_), K(skip_count), K(cost_time));
    }
  } else if (OB_FAIL(get_tenant_tx_data_mem_used_(tenant_tx_data_mem_used))) {
    LOG_WARN("[TenantFreezer] get tenant tx data mem used failed.", KR(ret));
  } else {
    int64_t total_memory = lib::get_tenant_memory_limit(tenant_info_.tenant_id_);
    int64_t hold_memory = lib::get_tenant_memory_hold(tenant_info_.tenant_id_);
    int64_t self_freeze_min_limit_ = total_memory * (ObTxDataTable::TX_DATA_FREEZE_TRIGGER_MIN_PERCENTAGE / 100);
    int64_t self_freeze_max_limit_ = total_memory * (ObTxDataTable::TX_DATA_FREEZE_TRIGGER_MAX_PERCENTAGE / 100);
    int64_t self_freeze_tenant_hold_limit_
      = (total_memory * (double(get_freeze_trigger_percentage_()) / 100));

    if ((tenant_tx_data_mem_used > self_freeze_max_limit_)
        || ((hold_memory > self_freeze_tenant_hold_limit_)
            && (tenant_tx_data_mem_used > self_freeze_min_limit_))) {
      // trigger tx data self freeze
      LOG_INFO("[TenantFreezer] Trigger Tx Data Table Self Freeze. ", K(tenant_info_.tenant_id_),
               K(tenant_tx_data_mem_used), K(self_freeze_max_limit_), K(hold_memory),
               K(self_freeze_tenant_hold_limit_), K(self_freeze_min_limit_));

      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = post_tx_data_freeze_request_())) {
        LOG_WARN("[TenantFreezer] fail to do tx data self freeze", K(tmp_ret),
                 K(tenant_info_.tenant_id_));
      }
    }
  }
  return ret;
}

int ObTenantFreezer::check_and_do_freeze()
{
  int ret = OB_SUCCESS;

  int64_t check_and_freeze_start_ts = ObTimeUtil::current_time();

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else if (OB_FAIL(check_and_freeze_normal_data_())) {
    LOG_WARN("[TenantFreezer] check and freeze normal data failed.", KR(ret));
  } else if (OB_FAIL(check_and_freeze_tx_data_())) {
    LOG_WARN("[TenantFreezer] check and freeze tx data failed.", KR(ret));
  }

  int64_t check_and_freeze_end_ts = ObTimeUtil::current_time();
  int64_t spend_time = check_and_freeze_end_ts - check_and_freeze_start_ts;
  if (spend_time > 2_s) {
    STORAGE_LOG(WARN, "check and do freeze spend too much time", K(spend_time));
  }
  return ret;
}

int ObTenantFreezer::retry_failed_major_freeze_(bool &triggered)
{
  int ret = OB_SUCCESS;

  if (get_retry_major_info().is_valid()) {
    LOG_INFO("A major freeze is needed due to previous failure");
    if (OB_FAIL(do_major_freeze_(get_retry_major_info().frozen_scn_))) {
      LOG_WARN("major freeze failed", K(ret));
    }
    triggered = true;
  }

  return ret;
}

int ObTenantFreezer::set_tenant_freezing()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    SpinRLockGuard guard(lock_);
    ATOMIC_AAF(&tenant_info_.freeze_cnt_, 1);
  }
  return ret;
}

int ObTenantFreezer::unset_tenant_freezing(const bool rollback_freeze_cnt)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    SpinRLockGuard guard(lock_);
    if (rollback_freeze_cnt) {
      if (ATOMIC_AAF(&tenant_info_.freeze_cnt_, -1) < 0) {
        tenant_info_.freeze_cnt_ = 0;
      }
    }
  }
  return ret;
}

int ObTenantFreezer::set_tenant_slow_freeze(
    const common::ObTabletID &tablet_id,
    const int64_t protect_clock)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    const uint64_t tenant_id = tenant_info_.tenant_id_;
    SpinRLockGuard guard(lock_);
    if (!tenant_info_.slow_freeze_) {
      bool success = ATOMIC_BCAS(&tenant_info_.slow_freeze_, false, true);
      if (success) {
        tenant_info_.slow_freeze_timestamp_ = ObTimeUtility::fast_current_time();
        tenant_info_.slow_freeze_min_protect_clock_ = protect_clock;
        tenant_info_.slow_tablet_ = tablet_id;
      }
    } else if (tenant_info_.slow_freeze_ &&
               tenant_info_.slow_freeze_min_protect_clock_ > protect_clock) {
      tenant_info_.slow_freeze_timestamp_ = ObTimeUtility::fast_current_time();
      tenant_info_.slow_freeze_min_protect_clock_ = protect_clock;
      tenant_info_.slow_tablet_ = tablet_id;
    }
  }
  return ret;
}

int ObTenantFreezer::unset_tenant_slow_freeze_()
{
  // NOTE: yuanyuan.cxf do not lock to prevent deadlock.
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = tenant_info_.tenant_id_;
  if (tenant_info_.slow_freeze_) {
    bool success = ATOMIC_BCAS(&tenant_info_.slow_freeze_, true, false);
    if (success) {
      tenant_info_.slow_freeze_timestamp_ = 0;
      tenant_info_.slow_freeze_min_protect_clock_ = INT64_MAX;
      tenant_info_.slow_tablet_.reset();
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("[TenantFreezer] Unexpected error", K(tenant_id), KR(ret));
    }
  }
  return ret;
}

int ObTenantFreezer::unset_tenant_slow_freeze()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    SpinRLockGuard guard(lock_);
    ret = unset_tenant_slow_freeze_();
  }
  return ret;
}

int ObTenantFreezer::unset_tenant_slow_freeze(const common::ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    const uint64_t tenant_id = tenant_info_.tenant_id_;
    SpinRLockGuard guard(lock_);
    if (tenant_info_.slow_freeze_ && tenant_info_.slow_tablet_ == tablet_id) {
      bool success = ATOMIC_BCAS(&tenant_info_.slow_freeze_, true, false);
      if (success) {
        tenant_info_.slow_freeze_timestamp_ = 0;
        tenant_info_.slow_freeze_min_protect_clock_ = INT64_MAX;
        tenant_info_.slow_tablet_.reset();
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("[TenantFreezer] Unexpected error", K(tenant_id), K(tablet_id), KR(ret));
      }
    }
  }
  return ret;
}

int ObTenantFreezer::set_tenant_mem_limit(
    const int64_t lower_limit,
    const int64_t upper_limit)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else if (OB_UNLIKELY(lower_limit < 0)
             || OB_UNLIKELY(upper_limit < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("[TenantFreezer] invalid argument", KR(ret), K(lower_limit), K(upper_limit));
  } else {
    int64_t freeze_trigger_percentage = get_freeze_trigger_percentage_();
    if ((NULL != config_) &&
        (((int64_t)(config_->memstore_limit_percentage)) > 100 ||
         ((int64_t)(config_->memstore_limit_percentage)) <= 0 ||
         freeze_trigger_percentage > 100 ||
         freeze_trigger_percentage <= 0)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("[TenantFreezer] memstore limit percent in ObServerConfig is invaild",
               "memstore limit percent",
               (int64_t)config_->memstore_limit_percentage,
               "minor freeze trigger percent",
               freeze_trigger_percentage,
               KR(ret));
    } else {
      const uint64_t tenant_id = tenant_info_.tenant_id_;
      SpinWLockGuard guard(lock_); // It should be possible to change to a read lock here, this lock is a structural lock, it is not appropriate to borrow
      int64_t memstore_freeze_trigger_limit = 0;
      tenant_info_.mem_lower_limit_ = lower_limit;
      tenant_info_.mem_upper_limit_ = upper_limit;
      if (NULL != config_) {
        int64_t tmp_var = upper_limit / 100;
        tenant_info_.mem_memstore_limit_ = tmp_var * config_->memstore_limit_percentage;
        if (OB_FAIL(get_freeze_trigger_(memstore_freeze_trigger_limit))) {
          LOG_WARN("[TenantFreezer] fail to get minor freeze trigger", KR(ret), K(tenant_id));
        }
      }
      tenant_info_.is_loaded_ = true;
      if (OB_SUCC(ret)) {
        LOG_INFO("[TenantFreezer] set tenant mem limit",
                 "tenant id", tenant_id,
                 "mem_lower_limit", lower_limit,
                 "mem_upper_limit", upper_limit,
                 "mem_memstore_limit", tenant_info_.mem_memstore_limit_,
                 "memstore_freeze_trigger_limit", memstore_freeze_trigger_limit,
                 "mem_tenant_limit", get_tenant_memory_limit(tenant_info_.tenant_id_),
                 "mem_tenant_hold", get_tenant_memory_hold(tenant_info_.tenant_id_),
                 "mem_memstore_used", get_tenant_memory_hold(tenant_info_.tenant_id_,
                                                             ObCtxIds::MEMSTORE_CTX_ID));
      }
    }
  }
  return ret;
}

int ObTenantFreezer::get_tenant_mem_limit(
    int64_t &lower_limit,
    int64_t &upper_limit) const
{
  int ret = OB_SUCCESS;
  lower_limit = 0;
  upper_limit = 0;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    const uint64_t tenant_id = tenant_info_.tenant_id_;
    SpinRLockGuard guard(lock_);
    if (false == tenant_info_.is_loaded_) {
      ret = OB_NOT_REGISTERED;
    } else {
      lower_limit = tenant_info_.mem_lower_limit_;
      upper_limit = tenant_info_.mem_upper_limit_;
    }
  }
  return ret;
}

int ObTenantFreezer::get_tenant_memstore_cond(
    int64_t &active_memstore_used,
    int64_t &total_memstore_used,
    int64_t &memstore_freeze_trigger,
    int64_t &memstore_limit,
    int64_t &freeze_cnt,
    const bool force_refresh)
{
  int ret = OB_SUCCESS;
  int64_t unused = 0;
  const int64_t refresh_interval = 100 * 1000; // 100 ms
  int64_t current_time = OB_TSC_TIMESTAMP.current_time();
  RLOCAL_INIT(int64_t, last_refresh_timestamp, 0);
  RLOCAL(int64_t, last_active_memstore_used);
  RLOCAL(int64_t, last_total_memstore_used);
  RLOCAL(int64_t, last_memstore_freeze_trigger);
  RLOCAL(int64_t, last_memstore_limit);
  RLOCAL(int64_t, last_freeze_cnt);

  active_memstore_used = 0;
  total_memstore_used = 0;
  memstore_freeze_trigger = 0;
  memstore_limit = 0;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else if (!force_refresh &&
             current_time - last_refresh_timestamp < refresh_interval) {
    active_memstore_used = last_active_memstore_used;
    total_memstore_used = last_total_memstore_used;
    memstore_freeze_trigger = last_memstore_freeze_trigger;
    memstore_limit = last_memstore_limit;
    freeze_cnt = last_freeze_cnt;
  } else {
    const uint64_t tenant_id = tenant_info_.tenant_id_;
    SpinRLockGuard guard(lock_);
    if (false == tenant_info_.is_loaded_) {
      ret = OB_ENTRY_NOT_EXIST;
      LOG_INFO("[TenantFreezer] This tenant not exist", K(tenant_id), KR(ret));
    } else if (OB_FAIL(get_tenant_mem_usage_(active_memstore_used,
                                             total_memstore_used,
                                             unused))) {
      LOG_WARN("[TenantFreezer] failed to get tenant mem usage", KR(ret), K(tenant_id));
    } else if (OB_FAIL(get_freeze_trigger_(memstore_freeze_trigger))) {
        LOG_WARN("[TenantFreezer] fail to get minor freeze trigger", KR(ret), K(tenant_id));
    } else {
      memstore_limit = tenant_info_.mem_memstore_limit_;
      freeze_cnt = tenant_info_.freeze_cnt_;

      // cache the result
      last_refresh_timestamp = current_time;
      last_active_memstore_used = active_memstore_used;
      last_total_memstore_used = total_memstore_used;
      last_memstore_freeze_trigger = memstore_freeze_trigger;
      last_memstore_limit = memstore_limit;
      last_freeze_cnt = freeze_cnt;
    }
  }
  return ret;
}

int ObTenantFreezer::get_tenant_memstore_limit(int64_t &mem_limit)
{
  int ret = OB_SUCCESS;
  mem_limit = INT64_MAX;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    const uint64_t tenant_id = tenant_info_.tenant_id_;
    SpinRLockGuard guard(lock_);
    if (false == tenant_info_.is_loaded_) {
      mem_limit = INT64_MAX;
      LOG_INFO("[TenantFreezer] This tenant not exist", K(tenant_id), KR(ret));
    } else {
      mem_limit = tenant_info_.mem_memstore_limit_;
    }
  }
  return ret;
}

int ObTenantFreezer::get_tenant_mem_usage_(
    int64_t &active_memstore_used,
    int64_t &total_memstore_used,
    int64_t &total_memstore_hold)
{
  int ret = OB_SUCCESS;
  ObTenantMemstoreAllocator *tenant_allocator = NULL;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    const uint64_t tenant_id = tenant_info_.tenant_id_;
    if (OB_FAIL(allocator_mgr_->get_tenant_memstore_allocator(tenant_id,
                                                              tenant_allocator))) {
      LOG_WARN("[TenantFreezer] failed to get_tenant_memstore_allocator", KR(ret), K(tenant_id));
    } else if (NULL == tenant_allocator) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("[TenantFreezer] tenant memstore allocator is NULL", KR(ret), K(tenant_id));
    } else {
      active_memstore_used = tenant_allocator->get_mem_active_memstore_used();
      total_memstore_used = tenant_allocator->get_mem_total_memstore_used();
      total_memstore_hold = get_tenant_memory_hold(tenant_id,
                                                   ObCtxIds::MEMSTORE_CTX_ID);
    }
  }

  return ret;
}

int ObTenantFreezer::get_freeze_trigger_(int64_t &memstore_freeze_trigger)
{
  int64_t not_used = 0;
  int64_t not_used2 = 0;

  return get_freeze_trigger_(not_used,
                             not_used2,
                             memstore_freeze_trigger);
}

static inline bool is_add_overflow(int64_t first, int64_t second, int64_t &res)
{
  if (first + second < 0) {
    return true;
  } else {
    res = first + second;
    return false;
  }
}

int ObTenantFreezer::get_mem_remain_trigger_(
    int64_t &mem_remain_trigger)
{
  int ret = OB_SUCCESS;
  const int64_t tenant_id = tenant_info_.tenant_id_;
  omt::ObTenantConfigGuard tenant_config(TENANT_CONF(tenant_id));
  double memstore_limit = tenant_info_.mem_memstore_limit_;

  // 1. trigger by write throttling
  if (!tenant_config.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[TenantFreezer] failed to get tenant config", KR(ret));
  } else {
    int64_t trigger_percentage = tenant_config->writing_throttling_trigger_percentage;
    mem_remain_trigger = memstore_limit * (100 - trigger_percentage) / 100 / 0.95;
  }
  return ret;
}

int ObTenantFreezer::get_freeze_trigger_(
    /* Now the maximum memory size that the memstore module can preempt and obtain */
    int64_t &max_mem_memstore_can_get_now,
    int64_t &kv_cache_mem,
    int64_t &memstore_freeze_trigger)
{
  int ret = OB_SUCCESS;
  ObTenantResourceMgrHandle resource_handle;
  const uint64_t tenant_id = tenant_info_.tenant_id_;
  const int64_t mem_memstore_limit = tenant_info_.mem_memstore_limit_;
  if (OB_UNLIKELY(NULL == config_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("[TenantFreezer] config_ is nullptr", KR(ret), K(tenant_id));
  } else if (OB_FAIL(ObResourceMgr::get_instance().
                    get_tenant_resource_mgr(tenant_id,
                                            resource_handle))) {
    LOG_WARN("[TenantFreezer] fail to get resource mgr", KR(ret), K(tenant_id));
    ret = OB_SUCCESS;
    memstore_freeze_trigger =
      get_freeze_trigger_percentage_() / 100 * mem_memstore_limit;
  } else {
    int64_t tenant_mem_limit = get_tenant_memory_limit(tenant_id);
    int64_t tenant_mem_hold = get_tenant_memory_hold(tenant_id);
    int64_t tenant_memstore_hold = get_tenant_memory_hold(tenant_id,
                                                          ObCtxIds::MEMSTORE_CTX_ID);
    bool is_overflow = true;
    kv_cache_mem = resource_handle.get_memory_mgr()->get_cache_hold();
    if (tenant_mem_limit < tenant_mem_hold) {
      LOG_WARN("[TenantFreezer] tenant_mem_limit is smaller than tenant_mem_hold",
               K(tenant_mem_limit), K(tenant_mem_hold), K(tenant_id));
    } else if (is_add_overflow(tenant_mem_limit - tenant_mem_hold,
                               tenant_memstore_hold,
                               max_mem_memstore_can_get_now)) {
      if (REACH_TIME_INTERVAL(1 * 1000 * 1000)) {
        LOG_WARN("[TenantFreezer] max memstore can get is overflow", K(tenant_mem_limit),
                 K(tenant_mem_hold), K(tenant_memstore_hold), K(tenant_id));
      }
    } else if (is_add_overflow(max_mem_memstore_can_get_now,
                               kv_cache_mem,
                               max_mem_memstore_can_get_now)) {
      if (REACH_TIME_INTERVAL(1 * 1000 * 1000)) {
        LOG_WARN("[TenantFreezer] max memstore can get is overflow",
                 K(tenant_mem_limit), K(tenant_mem_hold), K(tenant_memstore_hold),
                 K(kv_cache_mem), K(tenant_id));
      }
    } else {
      is_overflow = false;
    }

    int64_t min = mem_memstore_limit;
    if (!is_overflow) {
      min = MIN(mem_memstore_limit, max_mem_memstore_can_get_now);
    }

    if (min < 100) {
      memstore_freeze_trigger =  get_freeze_trigger_percentage_() * min / 100;
    } else {
      memstore_freeze_trigger = min / 100 * get_freeze_trigger_percentage_();
    }
  }

  return ret;
}

int ObTenantFreezer::check_tenant_out_of_memstore_limit(bool &is_out_of_mem)
{
  int ret = OB_SUCCESS;
  const int64_t check_memstore_limit_interval = 1 * 1000 * 1000;
  RLOCAL(int64_t, last_check_timestamp);
  RLOCAL(bool, last_result);
  int64_t current_time = OB_TSC_TIMESTAMP.current_time();
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    const uint64_t tenant_id = tenant_info_.tenant_id_;
    if (!last_result &&
        current_time - last_check_timestamp < check_memstore_limit_interval) {
      // Check once when the last memory burst or tenant_id does not match or the interval reaches the threshold
      is_out_of_mem = false;
    } else {
      int64_t active_memstore_used = 0;
      int64_t total_memstore_used = 0;
      int64_t total_memstore_hold = 0;
      SpinRLockGuard guard(lock_);
      if (false == tenant_info_.is_loaded_) {
        is_out_of_mem = false;
        LOG_INFO("[TenantFreezer] This tenant not exist", K(tenant_id), KR(ret));
      } else if (OB_FAIL(get_tenant_mem_usage_(active_memstore_used,
                                               total_memstore_used,
                                               total_memstore_hold))) {
        LOG_WARN("[TenantFreezer] fail to get mem usage", KR(ret), K(tenant_info_.tenant_id_));
      } else {
        is_out_of_mem = (total_memstore_hold > tenant_info_.mem_memstore_limit_);
      }
      last_check_timestamp = current_time;
    }
  }

  if (OB_SUCC(ret)) {
    last_result = is_out_of_mem;
  }
  return ret;
}

bool ObTenantFreezer::tenant_need_major_freeze()
{
  int ret = OB_SUCCESS;
  bool bool_ret = false;
  int64_t active_memstore_used = 0;
  int64_t total_memstore_used = 0;
  int64_t total_memstore_hold = 0;
  int64_t memstore_freeze_trigger = 0;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("tenant manager not init", K(ret));
  } else {
    SpinRLockGuard guard(lock_);
    if (!tenant_info_.is_loaded_) {
      // do nothing
    } else if (OB_FAIL(get_freeze_trigger_(memstore_freeze_trigger))) {
      LOG_WARN("fail to get minor freeze trigger", K(ret), K(tenant_info_.tenant_id_));
    } else if (OB_FAIL(get_tenant_mem_usage_(active_memstore_used,
                                             total_memstore_used,
                                             total_memstore_hold))) {
      LOG_WARN("fail to get mem usage", K(ret), K(tenant_info_.tenant_id_));
    } else {
      bool_ret = need_freeze_(active_memstore_used,
                              memstore_freeze_trigger);
      if (bool_ret) {
        LOG_INFO("A major freeze is needed",
                 "active_memstore_used_",
                 active_memstore_used,
                 "memstore_freeze_trigger_limit_",
                 memstore_freeze_trigger,
                 "tenant_id",
                 tenant_info_.tenant_id_);
      }
    }
  }
  return bool_ret;
}

int64_t ObTenantFreezer::get_freeze_trigger_percentage_() const
{
  static const int64_t DEFAULT_FREEZE_TRIGGER_PERCENTAGE = 20;
  int64_t percent = DEFAULT_FREEZE_TRIGGER_PERCENTAGE;
  omt::ObTenantConfigGuard tenant_config(TENANT_CONF(MTL_ID()));
  if (tenant_config.is_valid()) {
    percent = tenant_config->freeze_trigger_percentage;
  }
  return percent;
}

int ObTenantFreezer::post_freeze_request_(
    const storage::ObFreezeType freeze_type,
    const int64_t try_frozen_scn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else {
    ObTenantFreezeArg arg;
    arg.freeze_type_ = freeze_type;
    arg.try_frozen_scn_ = try_frozen_scn;
    LOG_INFO("[TenantFreezer] post freeze request to remote", K(arg));
    if (OB_FAIL(rpc_proxy_.to(self_).by(tenant_info_.tenant_id_).post_freeze_request(arg, &tenant_mgr_cb_))) {
      LOG_WARN("[TenantFreezer] fail to post freeze request", K(arg), KR(ret));
    }
    LOG_INFO("[TenantFreezer] after freeze at remote");
  }
  return ret;
}

int ObTenantFreezer::post_tx_data_freeze_request_()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("tenant manager not init", KR(ret));
  } else {
    ObTenantFreezeArg arg;
    arg.freeze_type_ = ObFreezeType::TX_DATA_TABLE_FREEZE;
    if (OB_FAIL(rpc_proxy_.to(self_).by(tenant_info_.tenant_id_).post_freeze_request(arg, &tenant_mgr_cb_))) {
      LOG_WARN("[TenantFreezer] fail to post freeze request", K(arg), KR(ret));
    }
  }
  return ret;
}

int ObTenantFreezer::rpc_callback()
{
  int ret = OB_SUCCESS;
  LOG_INFO("[TenantFreezer] call back of tenant freezer request");
  return ret;
}

void ObTenantFreezer::reload_config()
{
  int ret = OB_SUCCESS;
  int64_t freeze_trigger_percentage = get_freeze_trigger_percentage_();
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else if (NULL == config_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[TenantFreezer] config_ shouldn't be null here", KR(ret), KP(config_));
  } else if (((int64_t)(config_->memstore_limit_percentage)) > 100
             || ((int64_t)(config_->memstore_limit_percentage)) <= 0
             || freeze_trigger_percentage > 100
             || freeze_trigger_percentage <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("[TenantFreezer] memstore limit percent in ObServerConfig is invalid",
             "memstore limit percent",
             (int64_t)config_->memstore_limit_percentage,
             "minor freeze trigger percent",
             freeze_trigger_percentage,
             KR(ret));
  } else {
    SpinWLockGuard guard(lock_); // It should be possible to change to a read lock here, this lock is a structural lock, it is not appropriate to borrow
    if (true == tenant_info_.is_loaded_) {
      int64_t tmp_var = tenant_info_.mem_upper_limit_ / 100;
      tenant_info_.mem_memstore_limit_ =
        tmp_var * config_->memstore_limit_percentage;
    }
  }
  if (OB_SUCCESS == ret) {
    LOG_INFO("[TenantFreezer] reload config for tenant freezer",
             "new memstore limit percent",
             (int64_t)config_->memstore_limit_percentage,
             "new minor freeze trigger percent",
             freeze_trigger_percentage);
  }
}

int ObTenantFreezer::print_tenant_usage(
    char *print_buf,
    int64_t buf_len,
    int64_t &pos)
{
  int ret = OB_SUCCESS;
  lib::ObMallocAllocator *mallocator = lib::ObMallocAllocator::get_instance();
  int64_t active_memstore_used = 0;
  int64_t total_memstore_used = 0;
  int64_t total_memstore_hold = 0;
  int64_t memstore_freeze_trigger_limit = 0;
  int64_t max_mem_memstore_can_get_now = 0;
  int64_t kv_cache_mem = 0;

  SpinWLockGuard guard(lock_);
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("[TenantFreezer] tenant manager not init", KR(ret));
  } else if (OB_FAIL(get_tenant_mem_usage_(active_memstore_used,
                                           total_memstore_used,
                                           total_memstore_hold))) {
    LOG_WARN("[TenantFreezer] fail to get mem usage", KR(ret), K(tenant_info_.tenant_id_));
  } else if (OB_FAIL(get_freeze_trigger_(max_mem_memstore_can_get_now,
                                         kv_cache_mem,
                                         memstore_freeze_trigger_limit))) {
    LOG_WARN("[TenantFreezer] get tenant minor freeze trigger error", KR(ret), K(tenant_info_.tenant_id_));
  } else {
    ret = databuff_printf(print_buf, buf_len, pos,
                          "[TENANT_MEMORY] "
                          "tenant_id=% '9ld "
                          "active_memstore_used=% '15ld "
                          "total_memstore_used=% '15ld "
                          "total_memstore_hold=% '15ld "
                          "memstore_freeze_trigger_limit=% '15ld "
                          "memstore_limit=% '15ld "
                          "mem_tenant_limit=% '15ld "
                          "mem_tenant_hold=% '15ld "
                          "mem_memstore_used=% '15ld "
                          "kv_cache_mem=% '15ld "
                          "max_mem_memstore_can_get_now=% '15ld\n",
                          tenant_info_.tenant_id_,
                          active_memstore_used,
                          total_memstore_used,
                          total_memstore_hold,
                          memstore_freeze_trigger_limit,
                          tenant_info_.mem_memstore_limit_,
                          get_tenant_memory_limit(tenant_info_.tenant_id_),
                          get_tenant_memory_hold(tenant_info_.tenant_id_),
                          get_tenant_memory_hold(tenant_info_.tenant_id_,
                                                 ObCtxIds::MEMSTORE_CTX_ID),
                          kv_cache_mem,
                          max_mem_memstore_can_get_now);
  }

  if (!OB_ISNULL(mallocator)) {
    mallocator->print_tenant_memory_usage(tenant_info_.tenant_id_);
    mallocator->print_tenant_ctx_memory_usage(tenant_info_.tenant_id_);
  }

  return ret;
}

int ObTenantFreezer::get_global_frozen_scn_(int64_t &frozen_scn)
{
  int ret = OB_SUCCESS;
  const int64_t tenant_id = tenant_info_.tenant_id_;
  int64_t tmp_frozen_scn = 0;

  if (OB_FAIL(rootserver::ObMajorFreezeHelper::get_frozen_scn(tenant_id, tmp_frozen_scn))) {
    LOG_WARN("get_frozen_scn failed", KR(ret), K(tenant_id));
  } else {
    frozen_scn = tmp_frozen_scn;
  }

  return ret;
}

bool ObTenantFreezer::need_freeze_(
    const int64_t active_memstore_used,
    const int64_t memstore_freeze_trigger)
{
  bool need_freeze = false;
  const int64_t tenant_id = tenant_info_.tenant_id_;
  // 1. trigger by active memstore used.
  if (active_memstore_used > memstore_freeze_trigger) {
    need_freeze = true;
    LOG_INFO("[TenantFreezer] A minor freeze is needed by active memstore used.",
             K(active_memstore_used), K(memstore_freeze_trigger), K(tenant_id));
  }
  return need_freeze;
}

bool ObTenantFreezer::is_major_freeze_turn_()
{
  const int64_t freeze_cnt = tenant_info_.freeze_cnt_;
  int64_t major_compact_trigger = INT64_MAX;
  omt::ObTenantConfigGuard tenant_config(TENANT_CONF(MTL_ID()));
  if (tenant_config.is_valid()) {
    major_compact_trigger = tenant_config->major_compact_trigger;
  }
  return (major_compact_trigger != 0 && freeze_cnt >= major_compact_trigger);
}

bool ObTenantFreezer::is_minor_need_slow_(
    const int64_t total_memstore_hold,
    const int64_t memstore_freeze_trigger)
{
  int ret = OB_SUCCESS;
  bool need_slow = false;
  if (tenant_info_.slow_freeze_) {
    need_slow = true;
    int64_t now = ObTimeUtility::fast_current_time();
    if (total_memstore_hold <= memstore_freeze_trigger) {
      // no need minor freeze
    } else if (now - tenant_info_.slow_freeze_timestamp_ >= SLOW_FREEZE_INTERVAL) {
      need_slow = false;
    } else {
      // no need minor freeze
    }
  }
  return need_slow;
}

int ObTenantFreezer::do_minor_freeze_(const int64_t active_memstore_used,
                                      const int64_t memstore_freeze_trigger)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  bool rollback_freeze_cnt = false;
  LOG_INFO("[TenantFreezer] A minor freeze is needed",
           "active_memstore_used_", active_memstore_used,
           "memstore_freeze_trigger", memstore_freeze_trigger,
           "mem_tenant_remain", get_tenant_memory_remain(MTL_ID()),
           "mem_tenant_limit", get_tenant_memory_limit(MTL_ID()),
           "mem_tenant_hold", get_tenant_memory_hold(MTL_ID()),
           "mem_memstore_used", get_tenant_memory_hold(MTL_ID(),
                                                       ObCtxIds::MEMSTORE_CTX_ID),
           "tenant_id", MTL_ID());

  if (OB_FAIL(set_tenant_freezing())) {
  } else {
    bool rollback_freeze_cnt = false;
    if (OB_FAIL(tenant_freeze())) {
      rollback_freeze_cnt = true;
      LOG_WARN("fail to minor freeze", K(ret));
    } else {
      LOG_INFO("finish tenant minor freeze", K(ret));
    }
    // clear freezing mark for tenant
    int tmp_ret = OB_SUCCESS;
    if (OB_UNLIKELY(OB_SUCCESS !=
                    (tmp_ret = unset_tenant_freezing(rollback_freeze_cnt)))) {
      LOG_WARN("unset tenant freezing mark failed", K(tmp_ret));
      if (OB_SUCC(ret)) {
        ret = tmp_ret;
      }
    }
  }

  return ret;
}

int ObTenantFreezer::do_major_if_need_(const bool need_freeze)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  int64_t frozen_scn = 0;
  int64_t curr_frozen_scn = 0;
  bool need_major = false;
  bool major_triggered = false;
  if (OB_TMP_FAIL(retry_failed_major_freeze_(major_triggered))) {
    LOG_WARN("fail to do major freeze due to previous failure", K(tmp_ret));
  }
  // update frozen scn
  if (OB_FAIL(get_global_frozen_scn_(frozen_scn))) {
    LOG_WARN("fail to get global frozen version", K(ret));
  } else {
    SpinRLockGuard guard(lock_);
    if (!tenant_info_.is_loaded_) {
      // do nothing
    } else if (0 != frozen_scn && OB_FAIL(tenant_info_.update_frozen_scn(frozen_scn))) {
      LOG_WARN("fail to update frozen version", K(ret), K(frozen_scn), K_(tenant_info));
    } else {
      need_major = (need_freeze &&
                    !major_triggered &&
                    is_major_freeze_turn_());
      curr_frozen_scn = tenant_info_.frozen_scn_;
    }
  }
  if (need_major) {
    if (OB_FAIL(do_major_freeze_(curr_frozen_scn))) {
      LOG_WARN("[TenantFreezer] fail to do major freeze", K(tmp_ret));
    } else {
      // do nothing
    }
  }
  return ret;
}

int ObTenantFreezer::do_major_freeze_(const int64_t try_frozen_scn)
{
  int ret = OB_SUCCESS;
  LOG_INFO("A major freeze is needed", K(try_frozen_scn));
  if (OB_FAIL(post_freeze_request_(MAJOR_FREEZE,
                                   try_frozen_scn))) {
    LOG_WARN("major freeze failed", K(ret), K_(tenant_info));
  }

  return ret;
}

void ObTenantFreezer::log_frozen_memstore_info_if_need_(
    const int64_t active_memstore_used,
    const int64_t total_memstore_used,
    const int64_t total_memstore_hold,
    const int64_t memstore_freeze_trigger)
{
  int ret = OB_SUCCESS;
  ObTenantMemstoreAllocator *tenant_allocator = NULL;
  if (total_memstore_hold > memstore_freeze_trigger) {
    // There is an unreleased memstable
    LOG_INFO("[TenantFreezer] tenant have inactive memstores",
             K(active_memstore_used),
             K(total_memstore_used),
             K(total_memstore_hold),
             "memstore_freeze_trigger_limit_",
             memstore_freeze_trigger,
             "tenant_id",
             MTL_ID());

    if (OB_FAIL(allocator_mgr_->get_tenant_memstore_allocator(MTL_ID(),
                                                              tenant_allocator))) {
      LOG_WARN("[TenantFreezer] get tenant memstore allocator failed", KR(ret));
    } else {
      char frozen_mt_info[DEFAULT_BUF_LENGTH];
      tenant_allocator->log_frozen_memstore_info(frozen_mt_info,
                                                 sizeof(frozen_mt_info));
      LOG_INFO("[TenantFreezer] oldest frozen memtable", "list", frozen_mt_info);
    }
  }
}

void ObTenantFreezer::halt_prewarm_if_need_(
    const int64_t memstore_freeze_trigger,
    const int64_t total_memstore_hold)
{
  int ret = OB_SUCCESS;
  // When the memory is tight, try to abort the warm-up to release memstore
  int64_t mem_danger_limit = tenant_info_.mem_memstore_limit_
    - ((tenant_info_.mem_memstore_limit_ - memstore_freeze_trigger) >> 2);
  if (total_memstore_hold > mem_danger_limit) {
    int64_t curr_ts = ObTimeUtility::current_time();
    if (curr_ts - tenant_info_.last_halt_ts_ > 10L * 1000L * 1000L) {
      if (OB_FAIL(svr_rpc_proxy_->to(self_).
                  halt_all_prewarming_async(tenant_info_.tenant_id_, NULL))) {
        LOG_WARN("[TenantFreezer] fail to halt prewarming", KR(ret), K(tenant_info_.tenant_id_));
      } else {
        tenant_info_.last_halt_ts_ = curr_ts;
      }
    }
  }
}

}
}
