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

#define USING_LOG_PREFIX RS

#include "rootserver/freeze/ob_zone_merge_manager.h"

#include "share/ob_zone_merge_table_operator.h"
#include "share/ob_global_merge_table_operator.h"
#include "observer/ob_server_struct.h"
#include "share/ob_cluster_version.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/ob_service_epoch_proxy.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "lib/utility/ob_macro_utils.h"
#include "rootserver/ob_rs_event_history_table_operator.h"

namespace oceanbase
{
namespace rootserver
{
using namespace oceanbase::common;
using namespace oceanbase::share;

ObZoneMergeManagerBase::ObZoneMergeManagerBase()
  : lock_(), is_inited_(false), is_loaded_(false),
    tenant_id_(common::OB_INVALID_ID), zone_count_(0),
    zone_merge_infos_(), global_merge_info_(), proxy_(NULL)
{}

int ObZoneMergeManagerBase::init(const uint64_t tenant_id, ObMySQLProxy &proxy)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", KR(ret));
  } else {
    tenant_id_ = tenant_id;
    proxy_ = &proxy;
    is_inited_ = true;
    is_loaded_ = false;
  }
  return ret;
}

int ObZoneMergeManagerBase::reload()
{
  int ret = OB_SUCCESS;

  ObSEArray<ObZone, DEFAULT_ZONE_COUNT> zone_list;
  HEAP_VAR(ObGlobalMergeInfo, global_merge_info) {
    ObMalloc alloc(ObModIds::OB_TEMP_VARIABLES);
    ObPtrGuard<ObZoneMergeInfo, common::MAX_ZONE_NUM> tmp_merge_infos(alloc);
    global_merge_info.tenant_id_ = tenant_id_;

    if (IS_NOT_INIT) {
      ret = OB_NOT_INIT;
      LOG_WARN("not init", KR(ret), K_(tenant_id));
    } else if (OB_FAIL(tmp_merge_infos.init())) {
      LOG_WARN("fail to alloc temp zone merge infos", KR(ret), K_(tenant_id));
    } else if (OB_FAIL(ObGlobalMergeTableOperator::load_global_merge_info(*proxy_, tenant_id_, global_merge_info))) {
      LOG_WARN("fail to get global merge info", KR(ret), K_(tenant_id));
    } else if (OB_FAIL(ObZoneMergeTableOperator::get_zone_list(*proxy_, tenant_id_, zone_list))) {
      LOG_WARN("fail to get zone list", KR(ret), K_(tenant_id));
    } else if (zone_list.count() > common::MAX_ZONE_NUM) {
      ret = OB_ERR_SYS;
      LOG_ERROR("the count of zone is more than limit, cannot reload",
                KR(ret), K_(tenant_id), "zone count", zone_list.count(),
                "zone count limit", common::MAX_ZONE_NUM);
    } else if (zone_list.empty()) {
      ret = OB_ERR_SYS;
      LOG_WARN("zone_list is empty", KR(ret), K_(tenant_id));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < zone_list.count(); ++i) {
        ObZoneMergeInfo &info = tmp_merge_infos.ptr()[i];
        info.zone_ = zone_list[i];
        info.tenant_id_ = tenant_id_;
        if (OB_FAIL(ObZoneMergeTableOperator::load_zone_merge_info(*proxy_, tenant_id_, info))) {
          LOG_WARN("fail to reload zone merge info", KR(ret), K_(tenant_id), "zone", zone_list[i]);
        }
      }
    }

    if (OB_SUCC(ret)) {
      reset_merge_info();
      if (OB_FAIL(global_merge_info_.assign(global_merge_info))) {
        LOG_WARN("fail to assign", KR(ret), K(global_merge_info));
      }

      for (int64_t i = 0; OB_SUCC(ret) && (i < zone_list.count()); ++i) {
        if (OB_FAIL(zone_merge_infos_[zone_count_].assign(tmp_merge_infos.ptr()[i]))) {
          LOG_WARN("fail to assign", KR(ret));
        }
        ++zone_count_;
      }
    }

    if (OB_SUCC(ret)) {
      is_loaded_ = true;
      LOG_INFO("succ to reload zone merge manager", K(zone_list));
    } else {
      LOG_WARN("fail to reload zone merge manager", KR(ret));
    }
  }
  return ret;
}

int ObZoneMergeManagerBase::try_reload()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K_(tenant_id));
  } else if (is_loaded_) {
  } else if (OB_FAIL(reload())) {
    LOG_WARN("fail to reload", KR(ret), K_(tenant_id));
  }
  return ret;
}

void ObZoneMergeManagerBase::reset_merge_info()
{
  zone_count_ = 0;
  global_merge_info_.reset();
  is_loaded_ = false;
}

int ObZoneMergeManagerBase::check_inner_stat() const
{
  int ret = OB_SUCCESS;
  if (!is_inited_ || !is_loaded_) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("inner_stat_error", K_(is_inited), K_(is_loaded), KR(ret));
  }
  return ret;
}

int ObZoneMergeManagerBase::check_freeze_service_epoch(
    ObMySQLTransaction &trans, 
    const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  int64_t persistent_epoch = -1;
  if (expected_epoch < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(expected_epoch));
  } else if (OB_FAIL(ObServiceEpochProxy::select_service_epoch_for_update(trans, tenant_id_,
             ObServiceEpochProxy::FREEZE_SERVICE_EPOCH, persistent_epoch))) {
    LOG_WARN("fail to select freeze_service_epoch for update", KR(ret), K_(tenant_id));
  } else if (persistent_epoch != expected_epoch) {
    ret = OB_FREEZE_SERVICE_EPOCH_MISMATCH;
    LOG_WARN("freeze service epoch mismatch", KR(ret), K(expected_epoch), K(persistent_epoch));
  }
  return ret;
}

void ObZoneMergeManagerBase::handle_trans_stat(
    ObMySQLTransaction &trans, 
    int &ret)
{
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(trans.end(OB_SUCC(ret)))) {
      LOG_WARN("trans end failed", "is_commit", OB_SUCC(ret), K(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
}

int ObZoneMergeManagerBase::is_in_merge(bool &merge) const
{
  int ret = OB_SUCCESS;
  merge = false;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    merge = global_merge_info_.is_in_merge();
  }
  return ret;
}

int ObZoneMergeManagerBase::is_merge_error(bool &merge_error) const
{
  int ret = OB_SUCCESS;
  merge_error = false;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    merge_error = global_merge_info_.is_merge_error_;
  }
  return ret;
}

int ObZoneMergeManagerBase::get_zone_merge_info(ObZoneMergeInfo &info) const
{
  int ret = OB_SUCCESS;
  if (tenant_id_ != info.tenant_id_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K_(tenant_id), K(info.tenant_id_));
  } else if (OB_FAIL(get_zone_merge_info(info.zone_, info))) {
    LOG_WARN("fail to get zone", KR(ret), K(info.zone_));
  }
  return ret;
}

int ObZoneMergeManagerBase::get_zone_merge_info(const int64_t idx, ObZoneMergeInfo &info) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K(idx), K_(tenant_id));
  } else if ((idx < 0) || (idx >= zone_count_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(idx), K_(tenant_id), K_(zone_count));
  } else if (OB_FAIL(info.assign(zone_merge_infos_[idx]))) {
    LOG_WARN("fail to assign", KR(ret), "info", zone_merge_infos_[idx]);
  }
  return ret;
}

int ObZoneMergeManagerBase::get_zone_merge_info(const ObZone &zone, ObZoneMergeInfo &info) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  int64_t idx = OB_INVALID_INDEX;
  if (OB_FAIL(check_valid(zone, idx))) {
    LOG_WARN("fail to check valid", KR(ret), K(zone), K_(tenant_id));
  } else if (OB_FAIL(info.assign(zone_merge_infos_[idx]))) {
    LOG_WARN("fail to assign", KR(ret), "info", zone_merge_infos_[idx]);
  }
 
  return ret;
}

int ObZoneMergeManagerBase::get_zone_merge_info(ObIArray<ObZoneMergeInfo> &infos) const
{
  int ret = OB_SUCCESS;
  infos.reset();
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && (i < zone_count_); ++i) {
      if (OB_FAIL(infos.push_back(zone_merge_infos_[i]))) {
        LOG_WARN("fail to add zone info list", KR(ret), K_(tenant_id));
      }
    }
  }
  return ret;
}

int ObZoneMergeManagerBase::get_zone(ObIArray<ObZone> &zone_list) const
{
  int ret = OB_SUCCESS;
  zone_list.reset();
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < zone_count_; ++i) {
      if (OB_FAIL(zone_list.push_back(zone_merge_infos_[i].zone_))) {
        LOG_WARN("fail to push back zone", KR(ret), K_(tenant_id));
      }
    }
  }
  return ret;
}

int ObZoneMergeManagerBase::get_snapshot(
    ObGlobalMergeInfo &global_merge_info,
    ObIArray<ObZoneMergeInfo> &info_array)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  global_merge_info.reset();
  info_array.reset();
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else if (OB_FAIL(global_merge_info.assign(global_merge_info_))) {
    LOG_WARN("fail to assign", KR(ret), K_(global_merge_info));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && (i < zone_count_); ++i) {
      if (OB_FAIL(info_array.push_back(zone_merge_infos_[i]))) {
        LOG_WARN("fail to push zone_merge_info", KR(ret), K_(tenant_id), "index", i);
      }
    }
  }
  return ret;
}

int ObZoneMergeManagerBase::start_zone_merge(
    const ObZone &zone,
    const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  int64_t idx = OB_INVALID_INDEX;
  ObMySQLTransaction trans;
  const int64_t cur_time = ObTimeUtility::current_time();
  const int64_t global_broadcast_scn = global_merge_info_.global_broadcast_scn_;
  const int64_t global_frozen_scn = global_merge_info_.frozen_scn_;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);

  if (OB_FAIL(check_valid(zone, idx))) {
    LOG_WARN("fail to check valid", KR(ret), K(zone), K_(tenant_id));
  } else if (zone_merge_infos_[idx].broadcast_scn_ >= global_broadcast_scn) {
    ret = OB_ERR_SYS;
    LOG_ERROR("broadcast_scn must not larger than global_broadcast_scn",
              "zone broadcast_scn", zone_merge_infos_[idx].broadcast_scn_,
              "global_broadcast_scn", global_broadcast_scn,
              KR(ret), K_(tenant_id), K(zone));
  } else if (zone_merge_infos_[idx].frozen_scn_ >= global_frozen_scn) {
    ret = OB_ERR_SYS;
    LOG_ERROR("frozen_scn must not larger than global_frozen_scn",
              "zone frozen_scn", zone_merge_infos_[idx].frozen_scn_,
              "global_frozen_scn", global_frozen_scn,
              KR(ret), K_(tenant_id), K(zone));
  } else if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
    LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
  } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
    LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
  } else {
    const int64_t is_merging = 1;
    ObZoneMergeInfo tmp_info;
    if (OB_FAIL(tmp_info.assign_value(zone_merge_infos_[idx]))) {
      LOG_WARN("fail to assign zone merge info", KR(ret), K(idx), "merge_info", zone_merge_infos_[idx]);
    } else {
      tmp_info.is_merging_.set_val(is_merging, true/*need_update*/);
      tmp_info.broadcast_scn_.set_val(global_broadcast_scn, true);
      tmp_info.frozen_scn_.set_val(global_frozen_scn, true);
      tmp_info.merge_start_time_.set_val(cur_time, true);
      tmp_info.merge_status_.set_val(ObZoneMergeInfo::MERGE_STATUS_MERGING, true);

      if (OB_FAIL(ObZoneMergeTableOperator::update_partial_zone_merge_info(trans, tenant_id_, tmp_info))) {
        LOG_WARN("fail to update partial zone merge info", KR(ret), K_(tenant_id), K(tmp_info));
      }

      handle_trans_stat(trans, ret);

      if (FAILEDx(zone_merge_infos_[idx].assign_value(tmp_info))) {
        LOG_WARN("fail to assign zone merge info", KR(ret), K(idx), K(tmp_info));
      } else {
        LOG_INFO("succ to update zone merge info", K_(tenant_id), "latest zone merge_info", tmp_info);
      }
    }
  }
  LOG_INFO("start zone merge", KR(ret), K_(tenant_id), K(zone), K(global_broadcast_scn));
  return ret;
}

int ObZoneMergeManagerBase::finish_zone_merge(
    const ObZone &zone,
    const int64_t expected_epoch,
    const int64_t last_merged_scn,
    const int64_t all_merged_scn)
{
  int ret = OB_SUCCESS;
  int64_t idx = OB_INVALID_INDEX;
  ObMySQLTransaction trans;
  const int64_t cur_time = ObTimeUtility::current_time();
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);

  if (OB_FAIL(check_valid(zone, idx))) {
    LOG_WARN("fail to check valid", KR(ret), K(zone), K_(tenant_id));
  } else if ((last_merged_scn <= 0) || (all_merged_scn <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(zone), K_(tenant_id),
             K(last_merged_scn), K(all_merged_scn));
  } else if ((last_merged_scn != zone_merge_infos_[idx].broadcast_scn_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_ERROR("invalid merged_scn", KR(ret), K(zone), K_(tenant_id),
              K(last_merged_scn), K(all_merged_scn), "zone broadcast_scn",
              zone_merge_infos_[idx].broadcast_scn_);
  } else if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
    LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
  } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
    LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
  } else {
    ObZoneMergeInfo tmp_info;
    if (OB_FAIL(tmp_info.assign_value(zone_merge_infos_[idx]))) {
      LOG_WARN("fail to assign zone merge info", KR(ret), K(idx), "merge_info", zone_merge_infos_[idx]);
    } else {
      ObZoneMergeInfo::MergeStatus status = static_cast<ObZoneMergeInfo::MergeStatus>(
        zone_merge_infos_[idx].merge_status_.value_);
      if (last_merged_scn > zone_merge_infos_[idx].last_merged_scn_) {
        const int64_t is_merging = 0;
        tmp_info.is_merging_.set_val(is_merging, true);
        tmp_info.last_merged_scn_.set_val(last_merged_scn, true);
        tmp_info.last_merged_time_.set_val(cur_time, true);
        status = ObZoneMergeInfo::MERGE_STATUS_IDLE;
        tmp_info.merge_status_.set_val(status, true);
      }

      if (all_merged_scn > zone_merge_infos_[idx].all_merged_scn_) {
        tmp_info.all_merged_scn_.set_val(all_merged_scn, true);
      }

      if (OB_FAIL(ObZoneMergeTableOperator::update_partial_zone_merge_info(trans, tenant_id_, tmp_info))) {
        LOG_WARN("fail to update partial zone merge info", KR(ret), K_(tenant_id), K(tmp_info));
      }

      handle_trans_stat(trans, ret);

      if (FAILEDx(zone_merge_infos_[idx].assign_value(tmp_info))) {
        LOG_WARN("fail to assign zone merge info", KR(ret), K(idx), K(tmp_info));
      } else {
        LOG_INFO("succ to update zone merge info", K_(tenant_id), "latest zone merge_info", tmp_info);
      }
    }
  }
  
  LOG_INFO("finish zone merge", KR(ret), K_(tenant_id), K(zone), K(last_merged_scn), K(all_merged_scn));
  return ret;
}

int ObZoneMergeManagerBase::suspend_merge(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  const bool is_suspend = true;
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else if (OB_FAIL(suspend_or_resume_zone_merge(is_suspend, expected_epoch))) {
    LOG_WARN("fail to suspend merge", KR(ret), K_(tenant_id), K(is_suspend), K(expected_epoch));
  }
  return ret;
}

int ObZoneMergeManagerBase::resume_merge(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  const bool is_suspend = false;
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else if (OB_FAIL(suspend_or_resume_zone_merge(is_suspend, expected_epoch))) {
    LOG_WARN("fail to resume merge", KR(ret), K_(tenant_id), K(is_suspend), K(expected_epoch));
  }
  return ret;
}

int ObZoneMergeManagerBase::set_merge_error(const int64_t error_type, const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  
  if ((error_type >= ObZoneMergeInfo::ERROR_TYPE_MAX)
      || (error_type < ObZoneMergeInfo::NONE_ERROR)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K_(tenant_id), K(error_type));
  } else {
    ObMySQLTransaction trans;
    const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
    int64_t is_merge_error = 1;
    if (error_type == ObZoneMergeInfo::NONE_ERROR) {
      is_merge_error = 0;
    }

    if (OB_FAIL(check_inner_stat())) {
      LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
    } else if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
      LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
    } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
      LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
    } else {
      ObGlobalMergeInfo tmp_global_info;
      if (OB_FAIL(tmp_global_info.assign_value(global_merge_info_))) {
        LOG_WARN("fail to assign global merge info", KR(ret), K_(global_merge_info));
      } else {
        tmp_global_info.is_merge_error_.set_val(is_merge_error, true);
        tmp_global_info.error_type_.set_val(error_type, true);

        if (OB_FAIL(ObGlobalMergeTableOperator::update_partial_global_merge_info(trans, tenant_id_, 
            tmp_global_info))) {
          LOG_WARN("fail to update partial global merge info", KR(ret), K(tmp_global_info));
        }

        handle_trans_stat(trans, ret);

        if (FAILEDx(global_merge_info_.assign_value(tmp_global_info))) {
          LOG_WARN("fail to assign global merge info", KR(ret), K(tmp_global_info));
        } else {
          LOG_INFO("succ to update global merge info", K_(tenant_id), "latest global merge_info", tmp_global_info);
        }
      }
    }
    
    if (OB_SUCC(ret)) {
      LOG_INFO("succ to set_merge_error", K_(tenant_id), K(error_type), K(global_merge_info_.is_merge_error_));
      ROOTSERVICE_EVENT_ADD("daily_merge", "set_merge_error", K_(tenant_id), K(is_merge_error), K(error_type));
    } 

  }
  return ret;
}

int ObZoneMergeManagerBase::set_zone_merging(
    const ObZone &zone,
    const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  int64_t idx = OB_INVALID_INDEX;
  ObMySQLTransaction trans;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
  if (OB_FAIL(check_valid(zone, idx))) {
    LOG_WARN("fail to check valid", KR(ret), K(zone), K_(tenant_id));
  } else if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
    LOG_WARN("fail to start transaction", KR(ret), K(zone), K_(tenant_id), K(meta_tenant_id));
  } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
    LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
  } else {
    const int64_t is_merging = 1;
    ObZoneMergeInfo tmp_info;
    if (OB_FAIL(tmp_info.assign_value(zone_merge_infos_[idx]))) {
      LOG_WARN("fail to assign zone merge info", KR(ret), K(idx), "merge_info", zone_merge_infos_[idx]);
    } else if (is_merging != zone_merge_infos_[idx].is_merging_) {
      tmp_info.is_merging_.set_val(is_merging, true);

      if (OB_FAIL(ObZoneMergeTableOperator::update_partial_zone_merge_info(trans, tenant_id_, tmp_info))) {
        LOG_WARN("fail to update partial zone merge info", KR(ret), K_(tenant_id), K(tmp_info));
      }

      handle_trans_stat(trans, ret);
      
      if (FAILEDx(zone_merge_infos_[idx].assign_value(tmp_info))) {
        LOG_WARN("fail to assign zone merge info", KR(ret), K(idx), K(tmp_info));
      } else {
        LOG_INFO("succ to update zone merge info", K_(tenant_id), "latest zone merge_info", tmp_info);
      }
    }
  }

  LOG_INFO("set zone merging", KR(ret), K(zone), K_(tenant_id));
  return ret;
}

int ObZoneMergeManagerBase::check_need_broadcast(
    const int64_t frozen_scn,
    bool &need_broadcast)
{
  int ret = OB_SUCCESS;
  need_broadcast = false;
  if (frozen_scn <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K_(tenant_id), K(frozen_scn));
  } else if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else if (global_merge_info_.frozen_scn_ < frozen_scn) {
    need_broadcast = true;
  }
  return ret;
}

int ObZoneMergeManagerBase::set_global_freeze_info(
    const int64_t frozen_scn,
    const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);

  bool need_broadcast = false;
  if (OB_FAIL(check_need_broadcast(frozen_scn, need_broadcast))) {
    LOG_WARN("fail to check_need_broadcast", KR(ret), K_(tenant_id), K(frozen_scn));
  } else if (!need_broadcast) {
    LOG_INFO("no need set global freeze info", K(frozen_scn), K_(global_merge_info));
  } else if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
    LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
  } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
    LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
  } else {
    ObGlobalMergeInfo tmp_global_info;
    if (OB_FAIL(tmp_global_info.assign_value(global_merge_info_))) {
      LOG_WARN("fail to assign global merge info", KR(ret), K_(tenant_id));
    } else {
      tmp_global_info.frozen_scn_.set_val(frozen_scn, true);
      if (OB_FAIL(ObGlobalMergeTableOperator::update_partial_global_merge_info(trans, tenant_id_, 
          tmp_global_info))) {
        LOG_WARN("fail to update partial global merge info", KR(ret), K(tmp_global_info));
      }

      handle_trans_stat(trans, ret);
      
      if (FAILEDx(global_merge_info_.assign_value(tmp_global_info))) {
        LOG_WARN("fail to assign global merge info", KR(ret), K(tmp_global_info));
      } else {
        LOG_INFO("succ to update global merge info", K_(tenant_id), "latest global merge_info", tmp_global_info);
      }
    }
  }

  LOG_INFO("finish set global freeze info", KR(ret), K_(tenant_id), K(frozen_scn), K(need_broadcast));
  return ret;
}

int ObZoneMergeManagerBase::get_global_broadcast_scn(int64_t &global_broadcast_scn) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    global_broadcast_scn = global_merge_info_.global_broadcast_scn_;
  }
  return ret;
}

int ObZoneMergeManagerBase::get_global_last_merged_scn(int64_t &global_last_merged_scn) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    global_last_merged_scn =  global_merge_info_.last_merged_scn_;
  }
  return ret;
}

int ObZoneMergeManagerBase::get_global_merge_status(ObZoneMergeInfo::MergeStatus &global_merge_status) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    global_merge_status = (ObZoneMergeInfo::MergeStatus)(global_merge_info_.merge_status_.value_);
  }
  return ret;
}

int ObZoneMergeManagerBase::get_global_last_merged_time(int64_t &global_last_merged_time) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    global_last_merged_time = global_merge_info_.last_merged_time_;
  }
  return ret;
}

int ObZoneMergeManagerBase::get_global_merge_start_time(int64_t &global_merge_start_time) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard guard(lock_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    global_merge_start_time = global_merge_info_.merge_start_time_;
  }
  return ret;
}

int ObZoneMergeManagerBase::generate_next_global_broadcast_scn(
    const int64_t expected_epoch,
    int64_t &next_scn)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else if (global_merge_info_.is_merge_error()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("should not be is_merge_error", KR(ret), K_(global_merge_info));
  } else if (global_merge_info_.last_merged_scn_ < global_merge_info_.global_broadcast_scn_) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("not merged yet", "last_merged_scn", global_merge_info_.last_merged_scn_.value_,
             "global_broadcast_scn", global_merge_info_.global_broadcast_scn_.value_, KR(ret),
             K_(tenant_id));
  } else if (global_merge_info_.last_merged_scn_ > global_merge_info_.global_broadcast_scn_) {
    ret = OB_ERR_SYS;
    LOG_ERROR("last_merged_scn must not larger than global_broadcast_scn", KR(ret),
              K_(tenant_id), "last_merged_scn", global_merge_info_.last_merged_scn_.value_,
              "global_broadcast_scn", global_merge_info_.global_broadcast_scn_.value_);
  } else if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
    LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
  } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
    LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
  } else {
    ObGlobalMergeInfo tmp_global_info;
    if (OB_FAIL(tmp_global_info.assign_value(global_merge_info_))) {
      LOG_WARN("fail to assign global merge info", KR(ret), K_(global_merge_info));
    } else {
      if (global_merge_info_.global_broadcast_scn_ < global_merge_info_.frozen_scn_) {
        // only when global_broadcast_scn is less than global frozen_scn, we can use 
        // frozen_scn to start major_freeze
        next_scn = global_merge_info_.frozen_scn_;
        tmp_global_info.global_broadcast_scn_.set_val(next_scn, true);
        const int64_t cur_time = ObTimeUtility::current_time();
        tmp_global_info.merge_start_time_.set_val(cur_time, true);
      } else if (global_merge_info_.global_broadcast_scn_ == global_merge_info_.frozen_scn_) {
        next_scn = global_merge_info_.global_broadcast_scn_;
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("global_broadcast_scn must not larger than global frozen_scn", KR(ret),
          K_(global_merge_info));
      }

      if (OB_SUCC(ret)) {
        LOG_INFO("next global_broadcast_scn", K_(tenant_id), K(next_scn), K(tmp_global_info));
        
        tmp_global_info.merge_status_.set_val(ObZoneMergeInfo::MERGE_STATUS_MERGING, true);
        if (OB_FAIL(ObGlobalMergeTableOperator::update_partial_global_merge_info(trans, tenant_id_, 
            tmp_global_info))) {
          LOG_WARN("fail to update partial global merge info", KR(ret), K(tmp_global_info));
        }
      }

      handle_trans_stat(trans, ret);

      if (FAILEDx(global_merge_info_.assign_value(tmp_global_info))) {
        LOG_WARN("fail to assign global merge info", KR(ret), K(tmp_global_info));
      } else {
        LOG_INFO("succ to update global merge info", K_(tenant_id), "latest global merge_info", tmp_global_info);
      }
    }
  }
  
  return ret;
}

// if all zones finished merge & checksum checking, we may need to update global merge info
int ObZoneMergeManagerBase::try_update_global_last_merged_scn(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else {
    // here, we don't check is_merge_error, cuz merge & chcksum already finished.
    // we need to do some update work at last. is_merge_error will be caught in next round
    const int64_t cur_time = ObTimeUtility::current_time();
    bool need_update = false;
    if (global_merge_info_.is_in_merge()) {
      bool already_merged = true;
      for (int64_t i = 0; OB_SUCC(ret) && already_merged && (i < zone_count_); ++i) {
        if (zone_merge_infos_[i].last_merged_scn_ < global_merge_info_.global_broadcast_scn_) {
          LOG_INFO("zone not merged", K_(tenant_id), "global_broadcast_scn",
                   global_merge_info_.global_broadcast_scn_.value_,
                   "zone last_merged_scn", zone_merge_infos_[i].last_merged_scn_.value_);
          already_merged = false;
        }
      }
      if (OB_SUCC(ret)) {
        need_update = already_merged;
      }
    }

    if (OB_SUCC(ret) && need_update) {
      if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
        LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
      } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
        LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
      } else {
        // after all zones finished merge, update global merge info
        ObGlobalMergeInfo tmp_global_info;
        if (OB_FAIL(tmp_global_info.assign_value(global_merge_info_))) {
          LOG_WARN("fail to assign global merge info", KR(ret), K_(global_merge_info));
        } else {
          const int64_t cur_time = ObTimeUtility::current_time();
          tmp_global_info.last_merged_time_.set_val(cur_time, true);
          tmp_global_info.last_merged_scn_.set_val(global_merge_info_.global_broadcast_scn_.value_, true);
          tmp_global_info.merge_status_.set_val(ObZoneMergeInfo::MERGE_STATUS_IDLE, true);

          if (OB_FAIL(ObGlobalMergeTableOperator::update_partial_global_merge_info(trans, tenant_id_, 
              tmp_global_info))) {
            LOG_WARN("fail to update partial global merge info", KR(ret), K(tmp_global_info));
          }

          handle_trans_stat(trans, ret);
          
          if (FAILEDx(global_merge_info_.assign_value(tmp_global_info))) {
            LOG_WARN("fail to assign global merge info", KR(ret), K_(tenant_id), K(tmp_global_info));
          } else {
            LOG_INFO("succ to update global merge info", K_(tenant_id), "latest global merge_info", tmp_global_info);
          }
        }
      }
    }
  }
  return ret;
}

// after finishing merge(before checksum checking), update global merge info
int ObZoneMergeManagerBase::update_global_merge_info_after_merge(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else if (global_merge_info_.is_in_verifying_status()) {
    LOG_INFO("already in verifying status, no need to update global merge status again", K_(tenant_id), 
             "global merge status", global_merge_info_.merge_status_);
  } else if (global_merge_info_.is_merge_error()) {
    ret = OB_INNER_STAT_ERROR;
    LOG_WARN("should not update global merge status, cuz is_merge_error is true", KR(ret), K_(global_merge_info));
  } else {
    if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
      LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
    } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
      LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
    } else {
      ObGlobalMergeInfo tmp_global_info;
      if (OB_FAIL(tmp_global_info.assign_value(global_merge_info_))) {
        LOG_WARN("fail to assign global merge info", KR(ret), K_(global_merge_info));
      } else {
        tmp_global_info.merge_status_.set_val(ObZoneMergeInfo::MERGE_STATUS_VERIFYING, true);
        if (OB_FAIL(ObGlobalMergeTableOperator::update_partial_global_merge_info(trans, tenant_id_, 
            tmp_global_info))) {
          LOG_WARN("fail to update partial global merge info", KR(ret), K(tmp_global_info));
        }

        handle_trans_stat(trans, ret);
        
        if (FAILEDx(global_merge_info_.assign_value(tmp_global_info))) {
          LOG_WARN("fail to assign global merge info", KR(ret), K_(tenant_id), K(tmp_global_info));
        } else {
          LOG_INFO("succ to update global merge info", K_(tenant_id), "latest global merge_info", tmp_global_info);
        }
      }
    }
  }
  return ret;
}

int ObZoneMergeManagerBase::try_update_zone_merge_info(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  ObArray<ObZone> to_delete_infos;
  ObArray<ObZoneMergeInfo> to_insert_infos;

  // 1. get zone_list of current tenant from __all_tenant when previous_locality is empty
  ObArray<ObZone> zone_list;
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K_(tenant_id));
  } else if (OB_FAIL(get_tenant_zone_list(zone_list))) {
    LOG_WARN("fail to get tenant zone list", KR(ret), K_(tenant_id));
  } else if (zone_list.count() > 0) {
    ObMySQLTransaction trans;
    ObArray<ObZoneMergeInfo> ori_merge_infos;
    const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);
    if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
      LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
    } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
      LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
    } else if (OB_FAIL(ObZoneMergeTableOperator::load_zone_merge_infos(trans, tenant_id_, ori_merge_infos))) {
      LOG_WARN("fail to load zone merge infos", KR(ret), K_(tenant_id));
    } else {
      // 2. delete row whose zone not exist in zone_list
      if (OB_FAIL(handle_zone_merge_info_to_delete(trans, ori_merge_infos, zone_list, to_delete_infos))) {
        LOG_WARN("fail to handle zone merge info to delete", KR(ret), K(ori_merge_infos), K(zone_list));
      // 3. insert row whose zone not exist in table
      } else if (OB_FAIL(handle_zone_merge_info_to_insert(trans, ori_merge_infos, zone_list, to_insert_infos))) {
        LOG_WARN("fail to handle zone merge info to insert", KR(ret), K(ori_merge_infos), K(zone_list));
      }
    }
    handle_trans_stat(trans, ret);

    if (OB_SUCC(ret) && ((to_delete_infos.count() > 0) || (to_insert_infos.count() > 0))) {
      LOG_INFO("succ to update zone info may caused by locality changing", K(to_delete_infos), K(to_insert_infos));
      if (OB_FAIL(reload())) {
        LOG_WARN("fail to reload after updating zone_merge_info", KR(ret));
      }
    }
  }
  
  return ret;
}

int ObZoneMergeManagerBase::check_valid(const ObZone &zone, int64_t &idx) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_inner_stat())) {
    LOG_WARN("fail to check inner stat", KR(ret), K(zone), K_(tenant_id));
  } else if (zone.is_empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(zone), K_(tenant_id));
  } else if (OB_FAIL(find_zone(zone, idx))) {
    LOG_WARN("fail to find_zone", KR(ret), K(zone), K_(tenant_id));
  }
  return ret;
}

int ObZoneMergeManagerBase::find_zone(const ObZone &zone, int64_t &idx) const
{
  int ret = OB_SUCCESS;
  idx = OB_INVALID_INDEX;
  for (int64_t i = 0; (i < zone_count_); ++i) {
    if (zone == zone_merge_infos_[i].zone_) {
      idx = i;
      break;
    }
  }

  if (idx < 0 || idx >= zone_count_) {
    ret = OB_ENTRY_NOT_EXIST;
  }
  return ret;
}

int ObZoneMergeManagerBase::suspend_or_resume_zone_merge(
    const bool is_suspend,
    const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  const int64_t cur_time = ObTimeUtility::current_time();
  ObMySQLTransaction trans;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id_);

  if (OB_FAIL(trans.start(proxy_, meta_tenant_id))) {
    LOG_WARN("fail to start transaction", KR(ret), K_(tenant_id), K(meta_tenant_id));
  } else if (OB_FAIL(check_freeze_service_epoch(trans, expected_epoch))) {
    LOG_WARN("fail to check freeze_service_epoch", KR(ret), K(expected_epoch));
  } else {
    ObGlobalMergeInfo tmp_global_info;
    if (OB_FAIL(tmp_global_info.assign_value(global_merge_info_))) {
      LOG_WARN("fail to assign global merge info", KR(ret), K_(global_merge_info));
    } else {
      tmp_global_info.suspend_merging_.set_val(is_suspend, true);
      if (OB_FAIL(ObGlobalMergeTableOperator::update_partial_global_merge_info(trans, tenant_id_, tmp_global_info))) {
        LOG_WARN("fail to update partial global merge info", KR(ret), K(tmp_global_info));
      }

      handle_trans_stat(trans, ret); 
      
      if (FAILEDx(global_merge_info_.assign_value(tmp_global_info))) {
        LOG_WARN("fail to assign global merge info", KR(ret), K(tmp_global_info));
      } else {
        LOG_INFO("succ to update global merge info", K_(tenant_id), "latest global merge_info", tmp_global_info);
      }
    }
  }

  return ret;
}

int ObZoneMergeManagerBase::get_tenant_zone_list(common::ObIArray<ObZone> &zone_list)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_ISNULL(proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", KR(ret), K_(tenant_id));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql.assign_fmt("SELECT * FROM %s WHERE tenant_id = '%lu' AND previous_locality = ''", 
          OB_ALL_TENANT_TNAME, tenant_id_))) {
        LOG_WARN("fail to append sql", KR(ret), K_(tenant_id));
      } else if (OB_FAIL(proxy_->read(res, OB_SYS_TENANT_ID, sql.ptr()))) {
        LOG_WARN("fail to execute sql", KR(ret), K_(tenant_id), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get sql result", KR(ret), K_(tenant_id), K(sql));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("fail to get next", KR(ret), K_(tenant_id), K(sql));
      } else {
        int64_t tmp_real_str_len = 0; // used to fill output argument
        SMART_VAR(char[MAX_ZONE_LIST_LENGTH], zone_list_str) {
          zone_list_str[0] = '\0';
          EXTRACT_STRBUF_FIELD_MYSQL(*result, "zone_list", zone_list_str,
                                    MAX_ZONE_LIST_LENGTH, tmp_real_str_len);
          if (FAILEDx(str2zone_list(zone_list_str, zone_list))) {
            LOG_WARN("fail to str2zone_list", KR(ret), K(zone_list_str));
          }
        }
      }

      int tmp_ret = OB_SUCCESS;
      if (OB_FAIL(ret)) {
        //nothing todo
      } else if (OB_ITER_END != (tmp_ret = result->next())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get more row than one", KR(ret), KR(tmp_ret), K(sql));
      }
    }
  }
  return ret;
}

int ObZoneMergeManagerBase::str2zone_list(
    const char *str,
    ObIArray<ObZone> &zone_list)
{
  int ret = OB_SUCCESS;
  char *item_str = NULL;
  char *save_ptr = NULL;
  zone_list.reuse();
  if (NULL == str) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("str is null", KP(str), K(ret));
  } else {
    while (OB_SUCC(ret)) {
      item_str = strtok_r((NULL == item_str ? const_cast<char *>(str) : NULL), ";", &save_ptr);
      if (NULL != item_str) {
        if (OB_FAIL(zone_list.push_back(ObZone(item_str)))) {
          LOG_WARN("fail to push_back", KR(ret));
        }
      } else {
        break;
      }
    }
  }
  return ret;
}

int ObZoneMergeManagerBase::handle_zone_merge_info_to_delete(
    ObMySQLTransaction &trans,
    const ObIArray<ObZoneMergeInfo> &ori_merge_infos,
    const ObIArray<ObZone> &zone_list,
    ObIArray<ObZone> &to_delete_infos)
{
  int ret = OB_SUCCESS;
  to_delete_infos.reuse();
  for (int64_t i = 0; (i < ori_merge_infos.count()) && OB_SUCC(ret); ++i) {
    bool exist = false;
    for (int64_t j = 0; (j < zone_list.count()) && OB_SUCC(ret) && !exist; ++j) {
      if (STRCMP(ori_merge_infos.at(i).zone_.ptr(), zone_list.at(j).ptr()) == 0) {
        exist = true;
      }
    }

    if (OB_SUCC(ret) && !exist) {
      if (OB_FAIL(to_delete_infos.push_back(ori_merge_infos.at(i).zone_))) {
        LOG_WARN("fail to push back", KR(ret), K_(tenant_id), "zone", ori_merge_infos.at(i).zone_);
      }
    }
  }

  if ((to_delete_infos.count() > 0) && OB_SUCC(ret)) {
    if (OB_FAIL(ObZoneMergeTableOperator::delete_tenant_merge_info_by_zone(trans, tenant_id_, to_delete_infos))) {
      LOG_WARN("fail to delete tenant zone merge info by zone", KR(ret), K_(tenant_id), K(to_delete_infos));
    }
  }
  return ret;
}

int ObZoneMergeManagerBase::handle_zone_merge_info_to_insert(
    ObMySQLTransaction &trans,
    const ObIArray<ObZoneMergeInfo> &ori_merge_infos,
    const ObIArray<ObZone> &zone_list,
    ObIArray<ObZoneMergeInfo> &to_insert_infos)
{
  int ret = OB_SUCCESS;
  to_insert_infos.reuse();
  for (int64_t i = 0; (i < zone_list.count()) && OB_SUCC(ret); ++i) {
    bool exist = false;
    for (int64_t j = 0; (j < ori_merge_infos.count()) && OB_SUCC(ret) && !exist; ++j) {
      if (STRCMP(ori_merge_infos.at(j).zone_.ptr(), zone_list.at(i).ptr()) == 0) {
        exist = true;
      }
    }

    if (OB_SUCC(ret) && !exist) {
      ObZoneMergeInfo tmp_info;
      tmp_info.tenant_id_ = tenant_id_;
      tmp_info.zone_ = zone_list.at(i);
      if (OB_FAIL(to_insert_infos.push_back(tmp_info))) {
        LOG_WARN("fail to push back", KR(ret), K_(tenant_id), K(tmp_info));
      }
    }
  }

  if ((to_insert_infos.count() > 0) && OB_SUCC(ret)) {
    if (OB_FAIL(ObZoneMergeTableOperator::insert_zone_merge_infos(trans, tenant_id_, to_insert_infos))) {
      LOG_WARN("fail to insert zone merge infos", KR(ret), K_(tenant_id), K(to_insert_infos));
    }
  }
  return ret;
}

// only used for copying data to/from shadow_
int ObZoneMergeManagerBase::copy_infos(
    ObZoneMergeManagerBase &dest, 
    const ObZoneMergeManagerBase &src)
{
  int ret = OB_SUCCESS;
  const int64_t count = src.zone_count_;
  if ((0 > count) || (common::MAX_ZONE_NUM < count)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid zone count", K(count), KR(ret));
  } else {
    for (int64_t idx = 0; (idx < count) && OB_SUCC(ret); ++idx) {
      if (OB_FAIL(dest.zone_merge_infos_[idx].assign(src.zone_merge_infos_[idx]))) {
        LOG_WARN("fail to assign", KR(ret), "info", src.zone_merge_infos_[idx]);
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(dest.global_merge_info_.assign(src.global_merge_info_))) {
        LOG_WARN("fail to assign", KR(ret), "info", src.global_merge_info_);
      }
    }
    if (OB_SUCC(ret)) {
      dest.zone_count_ = count;
      dest.is_inited_ = src.is_inited_;
      dest.is_loaded_ = src.is_loaded_;
    }
  }
  return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
ObZoneMergeManager::ObZoneMergeMgrGuard::ObZoneMergeMgrGuard(
    const SpinRWLock &lock,
    ObZoneMergeManagerBase &zone_merge_mgr,
    ObZoneMergeManagerBase &shadow,
    int &ret)
    :  lock_(const_cast<SpinRWLock &>(lock)), zone_merge_mgr_(zone_merge_mgr),
       shadow_(shadow), ret_(ret) 
{
  SpinRLockGuard copy_guard(lock_);
  int tmp_ret = OB_SUCCESS;
  if (OB_UNLIKELY(OB_SUCCESS != ret_)) {
  } else if (OB_UNLIKELY(OB_SUCCESS !=
      (tmp_ret = ObZoneMergeManager::copy_infos(shadow_, zone_merge_mgr_)))) {
    LOG_WARN("fail to copy to zone_merge_mgr shadow", K(tmp_ret), K_(ret));
  }
  if (OB_UNLIKELY(OB_SUCCESS != tmp_ret)) {
    ret_ = tmp_ret;
  }
}

ObZoneMergeManager::ObZoneMergeMgrGuard::~ObZoneMergeMgrGuard()
{
  SpinWLockGuard copy_guard(lock_);
  int tmp_ret = OB_SUCCESS;
  if (OB_UNLIKELY(OB_SUCCESS != ret_)) {
  } else if (OB_UNLIKELY(OB_SUCCESS !=
      (tmp_ret = ObZoneMergeManager::copy_infos(zone_merge_mgr_, shadow_)))) {
    LOG_WARN("fail to copy from zone_merge_mgr shadow", K(tmp_ret), K_(ret));
  }
  if (OB_UNLIKELY(OB_SUCCESS != tmp_ret)) {
    ret_ = tmp_ret;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
ObZoneMergeManager::ObZoneMergeManager()
  : write_lock_(), shadow_()
{}

ObZoneMergeManager::~ObZoneMergeManager()
{}

int ObZoneMergeManager::init(const uint64_t tenant_id, ObMySQLProxy &proxy)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObZoneMergeManagerBase::init(tenant_id, proxy))) {
    LOG_WARN("fail to init zone_merge_manager_base", KR(ret), K(tenant_id));
  } else if (OB_FAIL(shadow_.init(tenant_id, proxy))) {
    LOG_WARN("fail to init zone_merge_mgr_base shadow_", KR(ret), K(tenant_id));
  }
  return ret;
}

// TODO, donglou, eliminate duplicate code later
int ObZoneMergeManager::reload()
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.reload();
    }
  }
  return ret;
}

int ObZoneMergeManager::try_reload()
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.try_reload();
    }
  }
  return ret;
}

int ObZoneMergeManager::start_zone_merge(const ObZone &zone, const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.start_zone_merge(zone, expected_epoch);
    }
  }
  return ret;
}

int ObZoneMergeManager::finish_zone_merge(
    const ObZone &zone,
    const int64_t expected_epoch,
    const int64_t last_merged_scn,
    const int64_t all_merged_scn)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.finish_zone_merge(zone, expected_epoch, last_merged_scn, all_merged_scn);
    }
  }
  return ret;
}

int ObZoneMergeManager::suspend_merge(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.suspend_merge(expected_epoch);
    }
  }
  return ret;
}

int ObZoneMergeManager::resume_merge(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.resume_merge(expected_epoch);
    }
  }
  return ret;
}

int ObZoneMergeManager::set_merge_error(const int64_t merge_error, const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.set_merge_error(merge_error, expected_epoch);
    }
  }
  return ret;
}

int ObZoneMergeManager::set_zone_merging(const ObZone &zone, const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.set_zone_merging(zone, expected_epoch);
    }
  }
  return ret;
}

int ObZoneMergeManager::check_need_broadcast(
    const int64_t frozen_scn, 
    bool &need_broadcast)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.check_need_broadcast(frozen_scn, need_broadcast);
    }
  }
  return ret;
}

int ObZoneMergeManager::set_global_freeze_info(
    const int64_t frozen_scn,
    const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.set_global_freeze_info(frozen_scn, expected_epoch);
    }
  }
  return ret;
}

int ObZoneMergeManager::generate_next_global_broadcast_scn(
    const int64_t expected_epoch,
    int64_t &next_scn)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.generate_next_global_broadcast_scn(expected_epoch, next_scn);
    }
  }
  return ret;
}

int ObZoneMergeManager::try_update_global_last_merged_scn(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.try_update_global_last_merged_scn(expected_epoch);
    }
  }
  return ret;
}

int ObZoneMergeManager::update_global_merge_info_after_merge(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.update_global_merge_info_after_merge(expected_epoch);
    }
  }
  return ret;
}

int ObZoneMergeManager::try_update_zone_merge_info(const int64_t expected_epoch)
{
  int ret = OB_SUCCESS;
  SpinWLockGuard guard(write_lock_);
  // destruct shadow_copy_guard before return
  // otherwise the ret_ in shadow_copy_guard will never be returned
  {
    ObZoneMergeMgrGuard shadow_guard(lock_, 
      *(static_cast<ObZoneMergeManagerBase *> (this)), shadow_, ret);
    if (OB_SUCC(ret)) {
      ret = shadow_.try_update_zone_merge_info(expected_epoch);
    }
  }
  return ret;
}

} // namespace rootserver
} // namespace oceanbase
