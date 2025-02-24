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

#define USING_LOG_PREFIX SERVER

#include "obmp_base.h"

#include "lib/worker.h"
#include "lib/profile/ob_trace_id.h"
#include "lib/profile/ob_perf_event.h"
#include "lib/stat/ob_diagnose_info.h"
#include "lib/stat/ob_session_stat.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/utility.h"
#include "rpc/ob_request.h"
#include "rpc/obmysql/ob_mysql_packet.h"
#include "rpc/obmysql/packet/ompk_change_user.h"
#include "rpc/obmysql/packet/ompk_error.h"
#include "rpc/obmysql/packet/ompk_ok.h"
#include "rpc/obmysql/packet/ompk_eof.h"
#include "rpc/obmysql/packet/ompk_row.h"
#include "observer/mysql/obsm_row.h"
#include "observer/mysql/obsm_utils.h"            // ObSMUtils
#include "rpc/obmysql/ob_mysql_request_utils.h"
#include "share/config/ob_server_config.h"
#include "share/config/ob_server_config.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/client_feedback/ob_feedback_partition_struct.h"
#include "share/resource_manager/ob_resource_manager.h"
#include "sql/session/ob_sql_session_mgr.h"
#include "observer/mysql/obmp_utils.h"
#include "rpc/obmysql/obsm_struct.h"
#include "observer/mysql/ob_mysql_result_set.h"
#include "observer/mysql/ob_query_driver.h"
#include "share/config/ob_server_config.h"
#include "storage/tx/ob_trans_define.h"

namespace oceanbase
{
using namespace share;
using namespace rpc;
using namespace obmysql;
using namespace common;
using namespace transaction;
using namespace share::schema;
namespace sql
{
  class ObPiece;
}

namespace observer
{

ObMPBase::ObMPBase(const ObGlobalContext &gctx)
    : gctx_(gctx), process_timestamp_(0), proxy_version_(0)
{
}

ObMPBase::~ObMPBase()
{
  // wakeup_request内部会判断has_req_wakeup_标，
  // 这里调一次兜底异常路径忘记flush_buffer的场景
  if (!THIS_WORKER.need_retry()) {
    packet_sender_.finish_sql_request();
  }
}

int ObMPBase::response(const int retcode)
{
  UNUSED(retcode);
  int ret = OB_SUCCESS;
  if (!THIS_WORKER.need_retry()) {
    if (OB_FAIL(flush_buffer(true))) {
      LOG_WARN("failed to flush_buffer", K(ret));
    }
  }
  return ret;
}

int ObMPBase::setup_packet_sender()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(packet_sender_.init(req_))) {
    LOG_ERROR("packet sender init fail", KP(req_), K(ret));
    send_error_packet(ret, NULL);
  }
  return ret;
}

int ObMPBase::before_process()
{
  int ret = OB_SUCCESS;
  process_timestamp_ = common::ObTimeUtility::current_time();
  return ret;
}

int ObMPBase::update_transmission_checksum_flag(const ObSQLSessionInfo &session)
{
  return packet_sender_.update_transmission_checksum_flag(session);
}

int ObMPBase::update_proxy_sys_vars(ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  ObSMConnection* conn = get_conn();
  if (OB_UNLIKELY(NULL == conn)) {
    ret = OB_CONNECT_ERROR;
    LOG_WARN("connection in error, maybe has disconnected", K(ret));
  } else if (OB_FAIL(session.set_proxy_user_privilege(session.get_user_priv_set()))) {
    LOG_WARN("fail to set proxy user privilege system variables", K(ret));
  } else if (OB_FAIL(session.set_proxy_capability(conn->proxy_cap_flags_.capability_))) {
    LOG_WARN("fail to set proxy capability", K(ret));
  }
  return ret;
}

int ObMPBase::after_process(int error_code)
{
  int ret = OB_SUCCESS;
  if (!lib::is_diagnose_info_enabled()) {
  } else {
    NG_TRACE_EXT(process_end, OB_ID(run_ts), get_run_timestamp());
    const int64_t elapsed_time = common::ObTimeUtility::current_time() - get_receive_timestamp();
    bool is_slow = (elapsed_time > GCONF.trace_log_slow_query_watermark)
      && !THIS_WORKER.need_retry();
    if (is_slow) {
      if (THIS_WORKER.need_retry() && OB_TRY_LOCK_ROW_CONFLICT == error_code) {
        //如果是锁冲突，且接下来会重试，则不需要打印这条日志了
      } else {
        FORCE_PRINT_TRACE(THE_TRACE, "[slow query]");

        // slow query will flush cache
        FLUSH_TRACE();
      }
    } else if (can_force_print(error_code)) {
      // 需要打印TRACE日志的错误码添加在这里
      int process_ret = error_code;
      NG_TRACE_EXT(process_ret, Y(process_ret));
      FORCE_PRINT_TRACE(THE_TRACE, "[err query]");
    } else if (THIS_WORKER.need_retry()) {
      if (OB_TRY_LOCK_ROW_CONFLICT != error_code) {
        FORCE_PRINT_TRACE(THE_TRACE, "[packet retry query]");
      }
    } else {
      PRINT_TRACE(THE_TRACE);
    }
  }
  ObActiveSessionGuard::get_stat().exec_phase_ = 0;
  return ret;
}

void ObMPBase::cleanup()
{
  ObActiveSessionGuard::setup_default_ash();
}

void ObMPBase::disconnect()
{
  return packet_sender_.disconnect();
}

void ObMPBase::force_disconnect()
{
  return packet_sender_.force_disconnect();
}

int ObMPBase::clean_buffer()
{
  return packet_sender_.clean_buffer();
}

int ObMPBase::flush_buffer(const bool is_last)
{
  return packet_sender_.is_disable_response()? OB_SUCCESS: packet_sender_.flush_buffer(is_last);
}

ObSMConnection* ObMPBase::get_conn() const
{
  return packet_sender_.get_conn();
}

int ObMPBase::get_conn_id(uint32_t &sessid) const
{
  return packet_sender_.get_conn_id(sessid);
}

int ObMPBase::send_error_packet(int err,
                                const char* errmsg,
                                bool is_partition_hit /* = true */,
                                void *extra_err_info /* = NULL */)
{
  return packet_sender_.send_error_packet(err, errmsg, is_partition_hit, extra_err_info);
}

int ObMPBase::send_switch_packet(ObString &auth_name, ObString& auth_data)
{
  int ret = OB_SUCCESS;
  OMPKChangeUser packet;
  packet.set_auth_plugin_name(auth_name);
  packet.set_auth_response(auth_data);
  if (OB_FAIL(response_packet(packet, NULL))) {
    LOG_WARN("failed to send switch packet", K(packet), K(ret));
  }
  return ret;
}

int ObMPBase::load_system_variables(const ObSysVariableSchema &sys_variable_schema, ObSQLSessionInfo &session) const
{
  int ret = OB_SUCCESS;
  ObArenaAllocator calc_buf(ObModIds::OB_SQL_SESSION);
  for (int64_t i = 0; OB_SUCC(ret) && i < sys_variable_schema.get_sysvar_count(); ++i) {
    const ObSysVarSchema *sysvar = NULL;
    sysvar = sys_variable_schema.get_sysvar_schema(i);
    if (sysvar != NULL) {
      LOG_DEBUG("load system variable", K(*sysvar));
      if (OB_FAIL(session.load_sys_variable(calc_buf, sysvar->get_name(), sysvar->get_data_type(),
                                            sysvar->get_value(), sysvar->get_min_val(),
                                            sysvar->get_max_val(), sysvar->get_flags(), true))) {
        LOG_WARN("load sys variable failed", K(ret), K(*sysvar));
      }
    }
  }
  if (OB_SUCC(ret)) {
    //设置系统变量的最大版本号
    session.set_global_vars_version(sys_variable_schema.get_schema_version());
    //将影响plan的系统变量序列化并缓存
    if (OB_FAIL(session.gen_sys_var_in_pc_str())) {
      LOG_WARN("fail to gen sys var in pc str", K(ret));
    } else if (OB_FAIL(session.gen_configs_in_pc_str())) {
      LOG_WARN("fail to gen configs in pc string", K(ret));
    }
  }
  return ret;
}

int ObMPBase::send_ok_packet(ObSQLSessionInfo &session, ObOKPParam &ok_param, obmysql::ObMySQLPacket* pkt)
{
  return packet_sender_.send_ok_packet(session, ok_param);
}

int ObMPBase::send_eof_packet(const ObSQLSessionInfo &session, const ObMySQLResultSet &result)
{
  return packet_sender_.send_eof_packet(session, result);
}

int ObMPBase::create_session(ObSMConnection *conn, ObSQLSessionInfo *&sess_info)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(conn)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("get connection fail", K(ret));
  } else if (OB_ISNULL(gctx_.session_mgr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("session manager is null", K(ret));
  } else {
    if (OB_FAIL(gctx_.session_mgr_->create_session(conn, sess_info))) {
      LOG_WARN("create session fail", "sessid", conn->sessid_,
                "proxy_sessid", conn->proxy_sessid_, K(ret));
    } else {
      LOG_DEBUG("create session successfully", "sessid", conn->sessid_,
               "proxy_sessid", conn->proxy_sessid_);
      conn->is_sess_alloc_ = true;
      sess_info->set_user_session();
      sess_info->set_shadow(false);
      if (SQL_REQ_OP.get_sql_ssl_st(req_) != NULL) {
        sess_info->set_ssl_cipher(SSL_get_cipher_name((SSL*)SQL_REQ_OP.get_sql_ssl_st(req_)));
      } else {
        sess_info->set_ssl_cipher("");
      }
    }
  }
  return ret;
}

int ObMPBase::free_session()
{
  int ret = OB_SUCCESS;
  ObSMConnection* conn = NULL;
  if (NULL == (conn = packet_sender_.get_conn())) {
    ret = OB_CONNECT_ERROR;
    LOG_WARN("connection already disconnected", K(ret));
  } else if (OB_ISNULL(gctx_.session_mgr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("session manager is null", K(ret));
  } else {
    ObFreeSessionCtx ctx;
    ctx.tenant_id_ = conn->tenant_id_;
    ctx.sessid_ = conn->sessid_;
    ctx.proxy_sessid_ = conn->proxy_sessid_;
    ctx.has_inc_active_num_ = conn->has_inc_active_num_;
    if (OB_FAIL(gctx_.session_mgr_->free_session(ctx))) {
      LOG_WARN("fail to free session", K(ctx), K(ret));
    } else {
      LOG_INFO("free session successfully", K(ctx));
      conn->is_sess_free_ = true;
    }
  }
  return ret;
}

int ObMPBase::get_session(ObSQLSessionInfo *&sess_info)
{
  return packet_sender_.get_session(sess_info);
}

int ObMPBase::revert_session(ObSQLSessionInfo *sess_info)
{
  return packet_sender_.revert_session(sess_info);
}

int ObMPBase::init_process_var(sql::ObSqlCtx &ctx,
                               const ObMultiStmtItem &multi_stmt_item,
                               sql::ObSQLSessionInfo &session,
                               bool &use_trace_log) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(session.is_use_trace_log(use_trace_log))) {
    LOG_WARN("fail to get use_trace_log", K(ret));
  } else if (!packet_sender_.is_conn_valid()) {
    ret = OB_CONNECT_ERROR;
    LOG_WARN("connection already disconnected", K(ret));
  } else {
    const int64_t debug_sync_timeout = GCONF.debug_sync_timeout;
    // ignore session debug sync action actions to thread local actions error
    if (debug_sync_timeout > 0) {
      int tmp_ret = GDS.set_thread_local_actions(session.get_debug_sync_actions());
      if (OB_UNLIKELY(OB_SUCCESS != tmp_ret)) {
        LOG_WARN("set session debug sync actions to thread local actions failed", K(tmp_ret));
      }
    }
    // construct sql context
    ctx.multi_stmt_item_ = multi_stmt_item;
    ctx.session_info_ = &session;
    session.set_rpc_tenant_id(THIS_WORKER.get_rpc_tenant());
    const ObMySQLRawPacket &pkt = reinterpret_cast<const ObMySQLRawPacket&>(req_->get_packet());

    if (0 == multi_stmt_item.get_seq_num() && !session.is_in_transaction()) {
      // 第一条sql
      // 并且还没开启事务时，这条sql才能二次路由
      ctx.can_reroute_sql_ = (pkt.can_reroute_pkt() && get_conn()->is_support_proxy_reroute());
    }
    LOG_TRACE("recorded sql reroute flag", K(ctx.can_reroute_sql_));
  }
  return ret;
}

//外层调用会忽略do_after_process的错误码，因此这里将set_session_state的错误码返回也没有意义。
//因此这里忽略set_session_state错误码，warning buffer的reset和trace log 记录的流程不收影响。
int ObMPBase::do_after_process(sql::ObSQLSessionInfo &session,
                               bool use_session_trace,
                               sql::ObSqlCtx &ctx,
                               bool async_resp_used) const
{
  int ret = OB_SUCCESS;
  session.set_session_sleep();

  // reset warning buffers
  // 注意，此处req_has_wokenup_可能为true，不能再访问req对象
  // @todo 重构wb逻辑
  if (!async_resp_used) { // 异步回包不重置warning buffer，重置操作在callback中做
    session.reset_warnings_buf();
  }
  // clear tsi warning buffer
  ob_setup_tsi_warning_buffer(NULL);
  //trace end
  if (lib::is_diagnose_info_enabled()) {
    NG_TRACE(query_end);

    if (use_session_trace) {
      //不影响正常逻辑
      if (false == ctx.is_show_trace_stmt_) {//show trace语句的trace log不能show，保留之前执行语句的
        if (NULL != session.get_trace_buf()) {
          if (nullptr != THE_TRACE) {
            (void)session.get_trace_buf()->assign(*THE_TRACE);
          }
        }
      }
      FORCE_PRINT_TRACE(THE_TRACE, "[show trace]");
    } else if (ctx.force_print_trace_) {
      // query with TRACE_LOG hint can also use SHOW TRACE after its execution
      if (NULL != session.get_trace_buf()) {
        if (nullptr != THE_TRACE) {
          (void)session.get_trace_buf()->assign(*THE_TRACE);
        }
      }
      FORCE_PRINT_TRACE(THE_TRACE, "[trace hint]");
    }
  }
  return ret;
}

int ObMPBase::setup_user_resource_group(
    ObSMConnection &conn,
    const uint64_t tenant_id,
    const uint64_t user_id)
{
  int ret = OB_SUCCESS;
  uint64_t group_id = 0;
  if (!is_valid_tenant_id(tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid tenant", K(tenant_id), K(ret));
  } else if (OB_FAIL(G_RES_MGR.get_mapping_rule_mgr().get_group_id_by_user(
              tenant_id, user_id, group_id))) {
    LOG_WARN("fail get group id by user", K(user_id), K(tenant_id), K(ret));
  } else {
    // 将 group id 设置到调度层，之后这个 session 上的所有请求都是用这个 cgroup 的资源
    conn.group_id_ = group_id;
  }
  LOG_DEBUG("setup user resource group", K(user_id), K(tenant_id), K(ret));
  return ret;
}

// force refresh schema if local schema version < last schema version
int ObMPBase::check_and_refresh_schema(uint64_t login_tenant_id,
                                       uint64_t effective_tenant_id,
                                       ObSQLSessionInfo *session_info)
{
  int ret = OB_SUCCESS;
  int64_t local_version = 0;
  int64_t last_version = 0;

  if (login_tenant_id != effective_tenant_id) {
    // do nothing
    // https://aone.alibaba-inc.com/issue/18698167
  } else if (OB_ISNULL(gctx_.schema_service_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("null schema service", K(ret), K(gctx_));
  } else {
    bool need_revert_session = false;
    if (NULL == session_info) {
      if (OB_FAIL(get_session(session_info))) {
        LOG_WARN("get session failed");
      } else if (OB_ISNULL(session_info)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid session info", K(ret), K(session_info));
      } else {
        need_revert_session = true;
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(gctx_.schema_service_->get_tenant_refreshed_schema_version(effective_tenant_id, local_version))) {
        LOG_WARN("fail to get tenant refreshed schema version", K(ret));
      } else if (OB_FAIL(session_info->get_ob_last_schema_version(last_version))) {
        LOG_WARN("failed to get_sys_variable", K(OB_SV_LAST_SCHEMA_VERSION));
      } else if (local_version >= last_version) {
        // skip
      } else if (OB_FAIL(gctx_.schema_service_->async_refresh_schema(effective_tenant_id, last_version))) {
        LOG_WARN("failed to refresh schema", K(ret), K(effective_tenant_id), K(last_version));
      }
      if (need_revert_session && OB_LIKELY(NULL != session_info)) {
        revert_session(session_info);
      }
    }
  }
  return ret;
}

int ObMPBase::response_row(ObSQLSessionInfo &session,
                           common::ObNewRow &row,
                           const ColumnsFieldIArray *fields)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(fields) || row.get_count() != fields->count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("fields is null", K(ret), KP(fields));
  }
  lib::ContextParam param;
  param.set_mem_attr(session.get_effective_tenant_id(),
      ObModIds::OB_SQL_EXECUTOR, ObCtxIds::DEFAULT_CTX_ID)
    .set_properties(lib::USE_TL_PAGE_OPTIONAL)
    .set_page_size(OB_MALLOC_NORMAL_BLOCK_SIZE)
    .set_ablock_size(lib::INTACT_MIDDLE_AOBJECT_SIZE);
  CREATE_WITH_TEMP_CONTEXT(param) {
    for (int64_t i = 0; OB_SUCC(ret) && i < row.get_count(); ++i) {
      ObObj &value = row.get_cell(i);
      // need at ps mode
      if (value.get_type() != fields->at(i).type_.get_type()) {
        ObCastCtx cast_ctx(&THIS_WORKER.get_sql_arena_allocator(),
                            NULL, CM_WARN_ON_FAIL, fields->at(i).type_.get_collation_type());
        if (OB_FAIL(common::ObObjCaster::to_type(fields->at(i).type_.get_type(),
                                          cast_ctx,
                                          value,
                                          value))) {
          LOG_WARN("failed to cast object", K(ret), K(value), K(i),
                    K(value.get_type()), K(fields->at(i).type_.get_type()));
        }
      }
      ObCharsetType charset_type = CHARSET_INVALID;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(session.get_character_set_results(charset_type))) {
        LOG_WARN("fail to get result charset", K(ret));
      } else if (ob_is_string_type(value.get_type())
                  && CS_TYPE_INVALID != value.get_collation_type()
                  && OB_FAIL(value.convert_string_value_charset(charset_type,
                      THIS_WORKER.get_sql_arena_allocator()))) {
        LOG_WARN("convert string value charset failed", K(ret), K(value));
      } else if (value.is_clob_locator()
                  && OB_FAIL(ObQueryDriver::convert_lob_value_charset(value, charset_type,
                              THIS_WORKER.get_sql_arena_allocator()))) {
        LOG_WARN("convert lob value charset failed", K(ret));
      }
      if (OB_SUCC(ret) && OB_FAIL(ObQueryDriver::convert_lob_locator_to_longtext(value,
                                    session.is_client_use_lob_locator(),
                                    &THIS_WORKER.get_sql_arena_allocator()))) {
        LOG_WARN("convert lob locator to longtext failed", K(ret));
      }
    }

  }

  if (OB_SUCC(ret)) {
    const ObDataTypeCastParams dtc_params = ObBasicSessionInfo::create_dtc_params(&session);
    ObSMRow sm_row(obmysql::BINARY, row, dtc_params, fields);
    obmysql::OMPKRow rp(sm_row);
    if (OB_FAIL(response_packet(rp, &session))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("response packet fail", K(ret));
    }
  }
  return ret;
}
} // namespace observer
} // namespace oceanbase
