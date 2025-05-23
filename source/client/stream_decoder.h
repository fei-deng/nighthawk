#pragma once

#include <functional>

#include "envoy/common/time.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/event/dispatcher.h"
#include "envoy/http/conn_pool.h"
#include "envoy/server/tracer_config.h"

#include "nighthawk/common/operation_callback.h"
#include "nighthawk/common/request_source.h"
#include "nighthawk/common/statistic.h"

#include "external/envoy/source/common/common/random_generator.h"
#include "external/envoy/source/common/http/header_map_impl.h"
#include "external/envoy/source/common/stream_info/stream_info_impl.h"
#include "external/envoy/source/common/tracing/http_tracer_impl.h"

namespace Nighthawk {
namespace Client {

class StreamDecoderCompletionCallback {
public:
  virtual ~StreamDecoderCompletionCallback() = default;
  virtual void onComplete(bool success, const Envoy::Http::ResponseHeaderMap& headers) PURE;
  virtual void onPoolFailure(Envoy::Http::ConnectionPool::PoolFailureReason reason) PURE;
  virtual void exportLatency(const uint32_t response_code, const uint64_t latency_ns) PURE;
  virtual void handleResponseData(const Envoy::Buffer::Instance& response_data) PURE;
};

// TODO(oschaaf): create a StreamDecoderPool?

/**
 * A self destructing response decoder that discards the response body.
 */
class StreamDecoder : public Envoy::Http::ResponseDecoder,
                      public Envoy::Http::StreamCallbacks,
                      public Envoy::Http::ConnectionPool::Callbacks,
                      public Envoy::Event::DeferredDeletable,
                      public Envoy::Logger::Loggable<Envoy::Logger::Id::main> {
public:
  // TODO(pelintehcoder): Optimize request body handling by reconsidering the use of std::string.
  // - Evaluate whether absl::string_view or const std::string& can be used instead of storing full
  // copies.
  // - Ensure that lifetime concerns are properly handled to avoid dangling references.
  // - This will reduce unnecessary copies while maintaining safety.
  StreamDecoder(Envoy::Event::Dispatcher& dispatcher, Envoy::TimeSource& time_source,
                StreamDecoderCompletionCallback& decoder_completion_callback,
                OperationCallback caller_completion_callback, Statistic& connect_statistic,
                Statistic& latency_statistic, Statistic& response_header_sizes_statistic,
                Statistic& response_body_sizes_statistic, Statistic& origin_latency_statistic,
                HeaderMapPtr request_headers, std::string request_body, bool measure_latencies,
                uint32_t request_body_size, Envoy::Random::RandomGenerator& random_generator,
                Envoy::Tracing::TracerSharedPtr& tracer,
                absl::string_view latency_response_header_name)
      : dispatcher_(dispatcher), time_source_(time_source),
        decoder_completion_callback_(decoder_completion_callback),
        caller_completion_callback_(std::move(caller_completion_callback)),
        connect_statistic_(connect_statistic), latency_statistic_(latency_statistic),
        response_header_sizes_statistic_(response_header_sizes_statistic),
        response_body_sizes_statistic_(response_body_sizes_statistic),
        origin_latency_statistic_(origin_latency_statistic),
        request_headers_(std::move(request_headers)), request_body_(std::move(request_body)),
        connect_start_(time_source_.monotonicTime()), measure_latencies_(measure_latencies),
        request_body_size_(request_body_size),
        downstream_address_setter_(std::make_shared<Envoy::Network::ConnectionInfoSetterImpl>(
            // The two addresses aren't used in an execution of Nighthawk.
            /* downstream_local_address = */ nullptr, /* downstream_remote_address = */ nullptr)),
        stream_info_(time_source_, downstream_address_setter_,
                     Envoy::StreamInfo::FilterState::LifeSpan::FilterChain),
        random_generator_(random_generator), tracer_(tracer),
        latency_response_header_name_(latency_response_header_name) {
    if (measure_latencies_ && tracer_ != nullptr) {
      setupForTracing();
    }
    stream_info_.setUpstreamInfo(std::make_shared<Envoy::StreamInfo::UpstreamInfoImpl>());
  }

  // Http::StreamDecoder
  void decode1xxHeaders(Envoy::Http::ResponseHeaderMapPtr&&) override {}
  void decodeHeaders(Envoy::Http::ResponseHeaderMapPtr&& headers, bool end_stream) override;
  void decodeData(Envoy::Buffer::Instance&, bool end_stream) override;
  void decodeTrailers(Envoy::Http::ResponseTrailerMapPtr&& trailers) override;
  void decodeMetadata(Envoy::Http::MetadataMapPtr&&) override { PANIC("not implemented"); }
  void dumpState(std::ostream&, int) const override { PANIC("not implemented"); }

  // Http::StreamCallbacks
  void onResetStream(Envoy::Http::StreamResetReason reason,
                     absl::string_view transport_failure_reason) override;
  void onAboveWriteBufferHighWatermark() override {}
  void onBelowWriteBufferLowWatermark() override {}

  // ConnectionPool::Callbacks
  void onPoolFailure(Envoy::Http::ConnectionPool::PoolFailureReason reason,
                     absl::string_view transport_failure_reason,
                     Envoy::Upstream::HostDescriptionConstSharedPtr host) override;
  void onPoolReady(Envoy::Http::RequestEncoder& encoder,
                   Envoy::Upstream::HostDescriptionConstSharedPtr host,
                   Envoy::StreamInfo::StreamInfo& stream_info,
                   absl::optional<Envoy::Http::Protocol> protocol) override;

  static Envoy::StreamInfo::CoreResponseFlag
  streamResetReasonToResponseFlag(Envoy::Http::StreamResetReason reset_reason);
  void finalizeActiveSpan();
  void setupForTracing();

private:
  void onComplete(bool success);
  static const std::string& staticUploadContent() {
    static const auto s = new std::string(4194304, 'a');
    return *s;
  }

  Envoy::Event::Dispatcher& dispatcher_;
  Envoy::TimeSource& time_source_;
  StreamDecoderCompletionCallback& decoder_completion_callback_;
  OperationCallback caller_completion_callback_;
  Statistic& connect_statistic_;
  Statistic& latency_statistic_;
  Statistic& response_header_sizes_statistic_;
  Statistic& response_body_sizes_statistic_;
  Statistic& origin_latency_statistic_;
  HeaderMapPtr request_headers_;
  const std::string request_body_;
  Envoy::Http::ResponseHeaderMapPtr response_headers_;
  Envoy::Http::ResponseTrailerMapPtr trailer_headers_;
  const Envoy::MonotonicTime connect_start_;
  Envoy::MonotonicTime request_start_;
  bool complete_ = false;
  bool measure_latencies_;
  const uint32_t request_body_size_;
  Envoy::Tracing::EgressConfigImpl config_;
  std::shared_ptr<Envoy::Network::ConnectionInfoSetterImpl> downstream_address_setter_;
  Envoy::StreamInfo::StreamInfoImpl stream_info_;
  Envoy::Random::RandomGenerator& random_generator_;
  Envoy::Tracing::TracerSharedPtr& tracer_;
  Envoy::Tracing::SpanPtr active_span_;
  const std::string latency_response_header_name_;
};

} // namespace Client
} // namespace Nighthawk
