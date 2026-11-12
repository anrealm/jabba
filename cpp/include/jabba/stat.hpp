// SPDX-License-Identifier: MIT
// Per-run statistics and the request record (spec/semantics.md §5).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"

namespace jabba {

/// Bump when the record layout changes; consumers rely on it.
inline constexpr std::int64_t record_version = 1;

enum class NodeStatus { ok, failed, timeout, skipped };
enum class RunStatus { ok, failed, timeout, cancelled };
/// Normative category of a node error; the error text itself is informative only.
enum class ErrorKind { failed, timeout, deadline, cancelled };

inline std::string_view to_string(NodeStatus s) {
    switch (s) {
        case NodeStatus::ok: return "ok";
        case NodeStatus::failed: return "failed";
        case NodeStatus::timeout: return "timeout";
        case NodeStatus::skipped: return "skipped";
    }
    return "?";
}

inline std::string_view to_string(RunStatus s) {
    switch (s) {
        case RunStatus::ok: return "ok";
        case RunStatus::failed: return "failed";
        case RunStatus::timeout: return "timeout";
        case RunStatus::cancelled: return "cancelled";
    }
    return "?";
}

inline std::string_view to_string(ErrorKind k) {
    switch (k) {
        case ErrorKind::failed: return "failed";
        case ErrorKind::timeout: return "timeout";
        case ErrorKind::deadline: return "deadline";
        case ErrorKind::cancelled: return "cancelled";
    }
    return "?";
}

struct NodeStat {
    NodeStatus status = NodeStatus::skipped;
    std::optional<ErrorKind> error_kind;
    std::optional<std::string> error;
    /// Unix epoch seconds; empty if the node never started.
    std::optional<double> started_at;
    /// From start until the outcome was recorded; empty if the node never started.
    std::optional<double> duration_ms;
};

/// The finalized record of one run.
struct RequestStat {
    std::string request_id;
    std::string graph;
    RunStatus status = RunStatus::ok;
    double duration_ms = 0;
    /// In graph declaration order.
    std::vector<std::pair<std::string, NodeStat>> nodes;
    /// Written by nodes through Ctx::set; last write wins.
    JsonObject fields;

    const NodeStat* node(std::string_view name) const {
        for (auto& [n, s] : nodes) {
            if (n == name) return &s;
        }
        return nullptr;
    }

    const Json* field(std::string_view key) const {
        for (auto& [k, v] : fields) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    /// The schema-versioned record (spec/record.schema.json).
    Json to_record() const {
        auto opt = [](const auto& v) -> Json { return v ? Json(*v) : Json(); };
        JsonObject node_records;
        for (auto& [name, s] : nodes) {
            node_records.emplace_back(
                name, JsonObject{
                          {"status", std::string(to_string(s.status))},
                          {"error_kind", s.error_kind ? Json(std::string(to_string(*s.error_kind))) : Json()},
                          {"error", opt(s.error)},
                          {"started_at", opt(s.started_at)},
                          {"duration_ms", opt(s.duration_ms)},
                      });
        }
        return JsonObject{
            {"record_version", record_version},
            {"request_id", request_id},
            {"graph", graph},
            {"status", std::string(to_string(status))},
            {"duration_ms", duration_ms},
            {"nodes", std::move(node_records)},
            {"fields", fields},
        };
    }
};

}  // namespace jabba
