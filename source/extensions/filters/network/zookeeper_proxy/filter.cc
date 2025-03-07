#include "source/extensions/filters/network/zookeeper_proxy/filter.h"

#include <string>
#include <vector>

#include "envoy/config/core/v3/base.pb.h"
#include "envoy/extensions/filters/network/zookeeper_proxy/v3/zookeeper_proxy.pb.h"
#include "envoy/extensions/filters/network/zookeeper_proxy/v3/zookeeper_proxy.pb.validate.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/fmt.h"
#include "source/common/common/logger.h"
#include "source/common/stats/utility.h"
#include "source/extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ZooKeeperProxy {

ZooKeeperFilterConfig::ZooKeeperFilterConfig(
    const std::string& stat_prefix, const uint32_t max_packet_bytes,
    const bool enable_latency_threshold_metrics,
    const std::chrono::milliseconds default_latency_threshold,
    const LatencyThresholdOverrideList& latency_threshold_overrides, Stats::Scope& scope)
    : scope_(scope), max_packet_bytes_(max_packet_bytes), stats_(generateStats(stat_prefix, scope)),
      stat_name_set_(scope.symbolTable().makeSet("Zookeeper")),
      stat_prefix_(stat_name_set_->add(stat_prefix)), auth_(stat_name_set_->add("auth")),
      connect_latency_(stat_name_set_->add("connect_response_latency")),
      unknown_scheme_rq_(stat_name_set_->add("unknown_scheme_rq")),
      unknown_opcode_latency_(stat_name_set_->add("unknown_opcode_latency")),
      enable_latency_threshold_metrics_(enable_latency_threshold_metrics),
      default_latency_threshold_(default_latency_threshold),
      latency_threshold_override_map_(parseLatencyThresholdOverrides(latency_threshold_overrides)) {
  // https://zookeeper.apache.org/doc/r3.5.4-beta/zookeeperProgrammers.html#sc_BuiltinACLSchemes
  // lists commons schemes: "world", "auth", "digest", "host", "x509", and
  // "ip". These are used in filter.cc by appending "_rq".
  stat_name_set_->rememberBuiltins(
      {"auth_rq", "digest_rq", "host_rq", "ip_rq", "ping_response_rq", "world_rq", "x509_rq"});

  initOpCode(OpCodes::Ping, stats_.ping_resp_, stats_.ping_resp_fast_, stats_.ping_resp_slow_,
             "ping_response");
  initOpCode(OpCodes::SetAuth, stats_.auth_resp_, stats_.auth_resp_fast_, stats_.auth_resp_slow_,
             "auth_response");
  initOpCode(OpCodes::GetData, stats_.getdata_resp_, stats_.getdata_resp_fast_,
             stats_.getdata_resp_slow_, "getdata_resp");
  initOpCode(OpCodes::Create, stats_.create_resp_, stats_.create_resp_fast_,
             stats_.create_resp_slow_, "create_resp");
  initOpCode(OpCodes::Create2, stats_.create2_resp_, stats_.create2_resp_fast_,
             stats_.create2_resp_slow_, "create2_resp");
  initOpCode(OpCodes::CreateContainer, stats_.createcontainer_resp_,
             stats_.createcontainer_resp_fast_, stats_.createcontainer_resp_slow_,
             "createcontainer_resp");
  initOpCode(OpCodes::CreateTtl, stats_.createttl_resp_, stats_.createttl_resp_fast_,
             stats_.createttl_resp_slow_, "createttl_resp");
  initOpCode(OpCodes::SetData, stats_.setdata_resp_, stats_.setdata_resp_fast_,
             stats_.setdata_resp_slow_, "setdata_resp");
  initOpCode(OpCodes::GetChildren, stats_.getchildren_resp_, stats_.getchildren_resp_fast_,
             stats_.getchildren_resp_slow_, "getchildren_resp");
  initOpCode(OpCodes::GetChildren2, stats_.getchildren2_resp_, stats_.getchildren2_resp_fast_,
             stats_.getchildren2_resp_slow_, "getchildren2_resp");
  initOpCode(OpCodes::Delete, stats_.delete_resp_, stats_.delete_resp_fast_,
             stats_.delete_resp_slow_, "delete_resp");
  initOpCode(OpCodes::Exists, stats_.exists_resp_, stats_.exists_resp_fast_,
             stats_.exists_resp_slow_, "exists_resp");
  initOpCode(OpCodes::GetAcl, stats_.getacl_resp_, stats_.getacl_resp_fast_,
             stats_.getacl_resp_slow_, "getacl_resp");
  initOpCode(OpCodes::SetAcl, stats_.setacl_resp_, stats_.setacl_resp_fast_,
             stats_.setacl_resp_slow_, "setacl_resp");
  initOpCode(OpCodes::Sync, stats_.sync_resp_, stats_.sync_resp_fast_, stats_.sync_resp_slow_,
             "sync_resp");
  initOpCode(OpCodes::Check, stats_.check_resp_, stats_.check_resp_fast_, stats_.check_resp_slow_,
             "check_resp");
  initOpCode(OpCodes::Multi, stats_.multi_resp_, stats_.multi_resp_fast_, stats_.multi_resp_slow_,
             "multi_resp");
  initOpCode(OpCodes::Reconfig, stats_.reconfig_resp_, stats_.reconfig_resp_fast_,
             stats_.reconfig_resp_slow_, "reconfig_resp");
  initOpCode(OpCodes::SetWatches, stats_.setwatches_resp_, stats_.setwatches_resp_fast_,
             stats_.setwatches_resp_slow_, "setwatches_resp");
  initOpCode(OpCodes::SetWatches2, stats_.setwatches2_resp_, stats_.setwatches2_resp_fast_,
             stats_.setwatches2_resp_slow_, "setwatches2_resp");
  initOpCode(OpCodes::CheckWatches, stats_.checkwatches_resp_, stats_.checkwatches_resp_fast_,
             stats_.checkwatches_resp_slow_, "checkwatches_resp");
  initOpCode(OpCodes::RemoveWatches, stats_.removewatches_resp_, stats_.removewatches_resp_fast_,
             stats_.removewatches_resp_slow_, "removewatches_resp");
  initOpCode(OpCodes::GetEphemerals, stats_.getephemerals_resp_, stats_.getephemerals_resp_fast_,
             stats_.getephemerals_resp_slow_, "getephemerals_resp");
  initOpCode(OpCodes::GetAllChildrenNumber, stats_.getallchildrennumber_resp_,
             stats_.getallchildrennumber_resp_fast_, stats_.getallchildrennumber_resp_slow_,
             "getallchildrennumber_resp");
  initOpCode(OpCodes::Close, stats_.close_resp_, stats_.close_resp_fast_, stats_.close_resp_slow_,
             "close_resp");
}

ErrorBudgetResponseType
ZooKeeperFilterConfig::errorBudgetDecision(const OpCodes opcode,
                                           const std::chrono::milliseconds latency) const {
  if (!enable_latency_threshold_metrics_) {
    return ErrorBudgetResponseType::None;
  }
  // Set latency threshold for the current opcode.
  std::chrono::milliseconds latency_threshold = default_latency_threshold_;
  int32_t opcode_val = enumToSignedInt(opcode);
  auto it = latency_threshold_override_map_.find(opcode_val);
  if (it != latency_threshold_override_map_.end()) {
    latency_threshold = it->second;
  }

  // Determine fast/slow response based on the threshold.
  if (latency <= latency_threshold) {
    return ErrorBudgetResponseType::Fast;
  }

  return ErrorBudgetResponseType::Slow;
}

void ZooKeeperFilterConfig::initOpCode(OpCodes opcode, Stats::Counter& resp_counter,
                                       Stats::Counter& resp_fast_counter,
                                       Stats::Counter& resp_slow_counter, absl::string_view name) {
  OpCodeInfo& opcode_info = op_code_map_[opcode];
  opcode_info.resp_counter_ = &resp_counter;
  opcode_info.resp_fast_counter_ = &resp_fast_counter;
  opcode_info.resp_slow_counter_ = &resp_slow_counter;
  opcode_info.opname_ = std::string(name);
  opcode_info.latency_name_ = stat_name_set_->add(absl::StrCat(name, "_latency"));
}

int32_t ZooKeeperFilterConfig::getOpCodeIndex(LatencyThresholdOverride_Opcode opcode) {
  const OpcodeMap& opcode_map = opcodeMap();
  auto it = opcode_map.find(opcode);
  if (it != opcode_map.end()) {
    return it->second;
  }
  throw EnvoyException(fmt::format("Unknown opcode from config: {}", static_cast<int32_t>(opcode)));
}

LatencyThresholdOverrideMap ZooKeeperFilterConfig::parseLatencyThresholdOverrides(
    const LatencyThresholdOverrideList& latency_threshold_overrides) {
  LatencyThresholdOverrideMap latency_threshold_override_map;
  for (const auto& threshold_override : latency_threshold_overrides) {
    latency_threshold_override_map[getOpCodeIndex(threshold_override.opcode())] =
        std::chrono::milliseconds(PROTOBUF_GET_MS_REQUIRED(threshold_override, threshold));
  }
  return latency_threshold_override_map;
}

ZooKeeperFilter::ZooKeeperFilter(ZooKeeperFilterConfigSharedPtr config, TimeSource& time_source)
    : config_(std::move(config)), decoder_(createDecoder(*this, time_source)) {}

void ZooKeeperFilter::initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) {
  read_callbacks_ = &callbacks;
}

Network::FilterStatus ZooKeeperFilter::onData(Buffer::Instance& data, bool) {
  clearDynamicMetadata();
  return decoder_->onData(data);
}

Network::FilterStatus ZooKeeperFilter::onWrite(Buffer::Instance& data, bool) {
  clearDynamicMetadata();
  return decoder_->onWrite(data);
}

Network::FilterStatus ZooKeeperFilter::onNewConnection() { return Network::FilterStatus::Continue; }

DecoderPtr ZooKeeperFilter::createDecoder(DecoderCallbacks& callbacks, TimeSource& time_source) {
  return std::make_unique<DecoderImpl>(callbacks, config_->maxPacketBytes(), time_source);
}

void ZooKeeperFilter::setDynamicMetadata(const std::string& key, const std::string& value) {
  setDynamicMetadata({{key, value}});
}

void ZooKeeperFilter::clearDynamicMetadata() {
  envoy::config::core::v3::Metadata& dynamic_metadata =
      read_callbacks_->connection().streamInfo().dynamicMetadata();
  auto& metadata =
      (*dynamic_metadata.mutable_filter_metadata())[NetworkFilterNames::get().ZooKeeperProxy];
  metadata.mutable_fields()->clear();
}

void ZooKeeperFilter::setDynamicMetadata(
    const std::vector<std::pair<const std::string, const std::string>>& data) {
  envoy::config::core::v3::Metadata& dynamic_metadata =
      read_callbacks_->connection().streamInfo().dynamicMetadata();
  ProtobufWkt::Struct metadata(
      (*dynamic_metadata.mutable_filter_metadata())[NetworkFilterNames::get().ZooKeeperProxy]);
  auto& fields = *metadata.mutable_fields();

  for (const auto& pair : data) {
    auto val = ProtobufWkt::Value();
    val.set_string_value(pair.second);
    fields.insert({pair.first, val});
  }

  read_callbacks_->connection().streamInfo().setDynamicMetadata(
      NetworkFilterNames::get().ZooKeeperProxy, metadata);
}

void ZooKeeperFilter::onConnect(const bool readonly) {
  if (readonly) {
    config_->stats_.connect_readonly_rq_.inc();
    setDynamicMetadata("opname", "connect_readonly");
  } else {
    config_->stats_.connect_rq_.inc();
    setDynamicMetadata("opname", "connect");
  }
}

void ZooKeeperFilter::onDecodeError() {
  config_->stats_.decoder_error_.inc();
  setDynamicMetadata("opname", "error");
}

void ZooKeeperFilter::onRequestBytes(const uint64_t bytes) {
  config_->stats_.request_bytes_.add(bytes);
  setDynamicMetadata("bytes", std::to_string(bytes));
}

void ZooKeeperFilter::onResponseBytes(const uint64_t bytes) {
  config_->stats_.response_bytes_.add(bytes);
  setDynamicMetadata("bytes", std::to_string(bytes));
}

void ZooKeeperFilter::onPing() {
  config_->stats_.ping_rq_.inc();
  setDynamicMetadata("opname", "ping");
}

void ZooKeeperFilter::onAuthRequest(const std::string& scheme) {
  Stats::Counter& counter = Stats::Utility::counterFromStatNames(
      config_->scope_, {config_->stat_prefix_, config_->auth_,
                        config_->stat_name_set_->getBuiltin(absl::StrCat(scheme, "_rq"),
                                                            config_->unknown_scheme_rq_)});
  counter.inc();
  setDynamicMetadata("opname", "auth");
}

void ZooKeeperFilter::onGetDataRequest(const std::string& path, const bool watch) {
  config_->stats_.getdata_rq_.inc();
  setDynamicMetadata({{"opname", "getdata"}, {"path", path}, {"watch", watch ? "true" : "false"}});
}

void ZooKeeperFilter::onCreateRequest(const std::string& path, const CreateFlags flags,
                                      const OpCodes opcode) {
  std::string opname;

  switch (opcode) {
  case OpCodes::Create:
    opname = "create";
    config_->stats_.create_rq_.inc();
    break;
  case OpCodes::Create2:
    opname = "create2";
    config_->stats_.create2_rq_.inc();
    break;
  case OpCodes::CreateContainer:
    opname = "createcontainer";
    config_->stats_.createcontainer_rq_.inc();
    break;
  case OpCodes::CreateTtl:
    opname = "createttl";
    config_->stats_.createttl_rq_.inc();
    break;
  default:
    throw EnvoyException(fmt::format("Unknown opcode: {}", enumToSignedInt(opcode)));
    break;
  }

  setDynamicMetadata(
      {{"opname", opname}, {"path", path}, {"create_type", createFlagsToString(flags)}});
}

void ZooKeeperFilter::onSetRequest(const std::string& path) {
  config_->stats_.setdata_rq_.inc();
  setDynamicMetadata({{"opname", "setdata"}, {"path", path}});
}

void ZooKeeperFilter::onGetChildrenRequest(const std::string& path, const bool watch,
                                           const bool v2) {
  std::string opname = "getchildren";

  if (v2) {
    config_->stats_.getchildren2_rq_.inc();
    opname = "getchildren2";
  } else {
    config_->stats_.getchildren_rq_.inc();
  }

  setDynamicMetadata({{"opname", opname}, {"path", path}, {"watch", watch ? "true" : "false"}});
}

void ZooKeeperFilter::onDeleteRequest(const std::string& path, const int32_t version) {
  config_->stats_.delete_rq_.inc();
  setDynamicMetadata({{"opname", "delete"}, {"path", path}, {"version", std::to_string(version)}});
}

void ZooKeeperFilter::onExistsRequest(const std::string& path, const bool watch) {
  config_->stats_.exists_rq_.inc();
  setDynamicMetadata({{"opname", "exists"}, {"path", path}, {"watch", watch ? "true" : "false"}});
}

void ZooKeeperFilter::onGetAclRequest(const std::string& path) {
  config_->stats_.getacl_rq_.inc();
  setDynamicMetadata({{"opname", "getacl"}, {"path", path}});
}

void ZooKeeperFilter::onSetAclRequest(const std::string& path, const int32_t version) {
  config_->stats_.setacl_rq_.inc();
  setDynamicMetadata({{"opname", "setacl"}, {"path", path}, {"version", std::to_string(version)}});
}

void ZooKeeperFilter::onSyncRequest(const std::string& path) {
  config_->stats_.sync_rq_.inc();
  setDynamicMetadata({{"opname", "sync"}, {"path", path}});
}

void ZooKeeperFilter::onCheckRequest(const std::string&, const int32_t) {
  config_->stats_.check_rq_.inc();
}

void ZooKeeperFilter::onCheckWatchesRequest(const std::string& path, const int32_t) {
  config_->stats_.checkwatches_rq_.inc();
  setDynamicMetadata({{"opname", "checkwatches"}, {"path", path}});
}

void ZooKeeperFilter::onRemoveWatchesRequest(const std::string& path, const int32_t) {
  config_->stats_.removewatches_rq_.inc();
  setDynamicMetadata({{"opname", "removewatches"}, {"path", path}});
}

void ZooKeeperFilter::onMultiRequest() {
  config_->stats_.multi_rq_.inc();
  setDynamicMetadata("opname", "multi");
}

void ZooKeeperFilter::onReconfigRequest() {
  config_->stats_.reconfig_rq_.inc();
  setDynamicMetadata("opname", "reconfig");
}

void ZooKeeperFilter::onSetWatchesRequest() {
  config_->stats_.setwatches_rq_.inc();
  setDynamicMetadata("opname", "setwatches");
}

void ZooKeeperFilter::onSetWatches2Request() {
  config_->stats_.setwatches2_rq_.inc();
  setDynamicMetadata("opname", "setwatches2");
}

void ZooKeeperFilter::onGetEphemeralsRequest(const std::string& path) {
  config_->stats_.getephemerals_rq_.inc();
  setDynamicMetadata({{"opname", "getephemerals"}, {"path", path}});
}

void ZooKeeperFilter::onGetAllChildrenNumberRequest(const std::string& path) {
  config_->stats_.getallchildrennumber_rq_.inc();
  setDynamicMetadata({{"opname", "getallchildrennumber"}, {"path", path}});
}

void ZooKeeperFilter::onCloseRequest() {
  config_->stats_.close_rq_.inc();
  setDynamicMetadata("opname", "close");
}

void ZooKeeperFilter::onConnectResponse(const int32_t proto_version, const int32_t timeout,
                                        const bool readonly,
                                        const std::chrono::milliseconds& latency) {
  config_->stats_.connect_resp_.inc();

  switch (config_->errorBudgetDecision(OpCodes::Connect, latency)) {
  case ErrorBudgetResponseType::Fast:
    config_->stats_.connect_resp_fast_.inc();
    break;
  case ErrorBudgetResponseType::Slow:
    config_->stats_.connect_resp_slow_.inc();
    break;
  case ErrorBudgetResponseType::None:
    break;
  }

  Stats::Histogram& histogram = Stats::Utility::histogramFromElements(
      config_->scope_, {config_->stat_prefix_, config_->connect_latency_},
      Stats::Histogram::Unit::Milliseconds);
  histogram.recordValue(latency.count());

  setDynamicMetadata({{"opname", "connect_response"},
                      {"protocol_version", std::to_string(proto_version)},
                      {"timeout", std::to_string(timeout)},
                      {"readonly", std::to_string(readonly)}});
}

void ZooKeeperFilter::onResponse(const OpCodes opcode, const int32_t xid, const int64_t zxid,
                                 const int32_t error, const std::chrono::milliseconds& latency) {
  Stats::StatName opcode_latency = config_->unknown_opcode_latency_;
  std::string opname = "";
  auto iter = config_->op_code_map_.find(opcode);
  if (iter != config_->op_code_map_.end()) {
    const ZooKeeperFilterConfig::OpCodeInfo& opcode_info = iter->second;
    opcode_info.resp_counter_->inc();
    opcode_latency = opcode_info.latency_name_;
    opname = opcode_info.opname_;

    switch (config_->errorBudgetDecision(opcode, latency)) {
    case ErrorBudgetResponseType::Fast:
      opcode_info.resp_fast_counter_->inc();
      break;
    case ErrorBudgetResponseType::Slow:
      opcode_info.resp_slow_counter_->inc();
      break;
    case ErrorBudgetResponseType::None:
      break;
    }
  }

  Stats::Histogram& histogram = Stats::Utility::histogramFromStatNames(
      config_->scope_, {config_->stat_prefix_, opcode_latency},
      Stats::Histogram::Unit::Milliseconds);
  histogram.recordValue(latency.count());

  setDynamicMetadata({{"opname", opname},
                      {"xid", std::to_string(xid)},
                      {"zxid", std::to_string(zxid)},
                      {"error", std::to_string(error)}});
}

void ZooKeeperFilter::onWatchEvent(const int32_t event_type, const int32_t client_state,
                                   const std::string& path, const int64_t zxid,
                                   const int32_t error) {
  config_->stats_.watch_event_.inc();
  setDynamicMetadata({{"opname", "watch_event"},
                      {"event_type", std::to_string(event_type)},
                      {"client_state", std::to_string(client_state)},
                      {"path", path},
                      {"zxid", std::to_string(zxid)},
                      {"error", std::to_string(error)}});
}

} // namespace ZooKeeperProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
