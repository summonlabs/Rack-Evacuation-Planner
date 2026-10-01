// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared scenario builder for the test suites.  It renders the canonical
// scenario text format so a test only has to describe the one fact it is
// about; everything else stays consistent.

#ifndef REP_TESTS_PLANNER_FIXTURE_HPP
#define REP_TESTS_PLANNER_FIXTURE_HPP

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rep/rep.hpp>

namespace repfix {


struct Block {
  std::string kind;
  std::string authority;
  std::string stream;
  std::string generation;
  std::string epoch;
  std::string body;
  // An unbound block is present in the evidence bundle but no source
  // directive names it, so it is never evidence.
  bool unbound{false};
};

// Canonical stream identity per evidence kind, so a test that adds a block
// only has to state its body.
inline [[nodiscard]] Block default_block(const std::string& kind) {
  if (kind == "rack_composition") {
    return Block{kind, "rack-authority", "rack-A.comp", "7", "2", "", false};
  }
  if (kind == "enumeration") {
    return Block{kind, "rack-authority", "rack-A.enum", "7", "2", "", false};
  }
  if (kind == "obligation_catalog") {
    return Block{kind, "rack-authority", "rack-A.cat", "7", "2", "", false};
  }
  if (kind == "capacity") {
    return Block{kind, "facility-capacity", "capacity", "4", "1", "", false};
  }
  if (kind == "placement_policy") {
    return Block{kind, "placement-policy", "policy", "4", "1", "", false};
  }
  if (kind == "maintenance") {
    return Block{kind, "maintenance-authority", "maintenance", "4", "1", "", false};
  }
  if (kind == "failure_domain") {
    return Block{kind, "facility-capacity", "domains", "4", "1", "", false};
  }
  if (kind == "asi_workload_state") {
    return Block{kind, "agent-scheduler", "asi", "4", "1", "", false};
  }
  if (kind == "dfi_obligation") {
    return Block{kind, "data-fabric", "dfi", "4", "1", "", false};
  }
  return Block{kind, "agent-scheduler", "offers", "4", "1", "", false};
}

struct Scenario {
  std::string planner{"planner-01"};
  std::string key{"req-1"};
  std::string requester{"requester-01"};
  std::string expected_epoch{"0"};
  std::string time{"1000"};
  std::string lineage{"-"};
  std::string rack{"rack-A"};
  std::string isolation{"depower"};
  std::string revision{"7"};
  std::string kinds{"workload"};
  std::vector<Block> blocks;
  // Streams the request binds that no evidence block publishes.
  std::vector<Block> source_only;

  [[nodiscard]] std::string text() const {
    std::string out;
    out += "version 1\n";
    out += "planner " + planner + "\n";
    out += "request key=" + key + " authority=" + requester + " epoch=" + expected_epoch +
           " time=" + time + " lineage=" + lineage + " rack=" + rack +
           " isolation=" + isolation + " composition_revision=" + revision +
           " kinds=" + kinds + "\n";
    for (const Block& block : blocks) {
      if (block.unbound) {
        continue;
      }
      out += "source evidence=" + block.kind + " authority=" + block.authority +
             " stream=" + block.stream + "\n";
    }
    for (const Block& extra : source_only) {
      out += "source evidence=" + extra.kind + " authority=" + extra.authority +
             " stream=" + extra.stream + "\n";
    }
    for (const Block& block : blocks) {
      const std::string authority = block.unbound ? " authority=" + block.authority : std::string();
      out += "evidence " + block.kind + " stream=" + block.stream + authority +
             " generation=" + block.generation + " epoch=" + block.epoch + "\n";
      if (!block.body.empty()) {
        out += block.body;
        if (block.body.back() != '\n') {
          out += "\n";
        }
      }
      out += "end\n";
    }
    return out;
  }

  [[nodiscard]] Block* find(std::string_view kind) {
    for (Block& block : blocks) {
      if (block.kind == kind) {
        return &block;
      }
    }
    return nullptr;
  }

  // Inserts the block when it is absent, so a test that needs a stream only
  // has to describe its body.
  void set_body(const std::string& kind, std::string body) {
    Block* block = find(kind);
    if (block != nullptr) {
      block->body = std::move(body);
      return;
    }
    Block fresh = default_block(kind);
    fresh.body = std::move(body);
    blocks.push_back(std::move(fresh));
  }

  // Adds a record that stays in the bundle but is bound by no source, under a
  // stream name of its own so it cannot collide with the bound one.
  void add_unbound(std::string kind, std::string body) {
    Block fresh = default_block(kind);
    const auto stream = rep::StreamId::parse(fresh.stream + "-unbound");
    if (stream.ok()) {
      fresh.stream = stream.value().str();
    }
    fresh.body = std::move(body);
    fresh.unbound = true;
    blocks.push_back(std::move(fresh));
  }

  void remove(std::string_view kind) {
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                [kind](const Block& block) { return block.kind == kind; }),
                 blocks.end());
  }
};

inline [[nodiscard]] Block make_block(std::string kind, std::string authority, std::string stream,
                               std::string generation, std::string epoch, std::string body) {
  return Block{std::move(kind), std::move(authority), std::move(stream), std::move(generation),
               std::move(epoch), std::move(body)};
}

// One workload w1 in rack-A with a live-migrate candidate to rack-B, a
// complete enumeration, capacity, failure domains and ASI state.  Every test
// starts from this and breaks exactly the thing it is about.
inline [[nodiscard]] Scenario standard() {
  Scenario scenario;
  scenario.blocks.push_back(make_block(
      "rack_composition", "rack-authority", "rack-A.comp", "7", "2",
      "rack rack-A\nrevision 7\noccupants w1\n"));
  scenario.blocks.push_back(make_block(
      "enumeration", "rack-authority", "rack-A.enum", "7", "2",
      "rack rack-A\nrevision 7\ncomplete true\nenumerated w1\n"));
  scenario.blocks.push_back(make_block(
      "obligation_catalog", "rack-authority", "rack-A.cat", "7", "2",
      "obligation id=w1 kind=workload rack=rack-A demand=cpu_millicores:1000 protected=false "
      "depends=-\n"));
  scenario.blocks.push_back(make_block(
      "capacity", "facility-capacity", "capacity", "4", "1",
      "destination destination=rack_slot:rack-B available=cpu_millicores:8000\n"));
  scenario.blocks.push_back(make_block(
      "failure_domain", "facility-capacity", "domains", "4", "1",
      "member kind=rack rack=rack-A domain=fd-1\n"
      "member kind=destination destination=rack_slot:rack-B domain=fd-2\n"
      "member kind=obligation obligation=w1 domain=fd-1\n"));
  scenario.blocks.push_back(make_block(
      "asi_workload_state", "agent-scheduler", "asi", "4", "1",
      "record obligation=w1 lifecycle=running migration=live_allowed attachment=stateless\n"));
  scenario.blocks.push_back(make_block(
      "candidate_offers", "agent-scheduler", "offers", "4", "1",
      "candidate id=c1 obligation=w1 action=live_migrate destination=rack_slot:rack-B "
      "authority=agent-scheduler generation=4 epoch=1 provision=cpu_millicores:1000 cost=10 "
      "window=false\n"));
  return scenario;
}

inline [[nodiscard]] rep::Result<rep::PlanOutcome> plan_scenario(const std::string& text) {
  auto document = rep::parse_scenario(text);
  if (!document.ok()) {
    return document.error();
  }
  rep::EngineOptions options;
  options.planner = document.value().planner;
  auto engine = rep::PlanEngine::open(std::move(options));
  if (!engine.ok()) {
    return engine.error();
  }
  auto outcome = engine.value()->submit(document.value().request);
  if (!outcome.ok()) {
    return outcome.error();
  }
  return outcome.take();
}

inline [[nodiscard]] bool has_rejection(const rep::Residual& residual, rep::RejectionReason reason) {
  for (const rep::RejectedCandidate& rejected : residual.rejected) {
    if (rejected.reason == reason) {
      return true;
    }
  }
  return false;
}

inline [[nodiscard]] const rep::Residual* find_residual(const rep::Plan& plan, std::string_view obligation) {
  for (const rep::Residual& residual : plan.residuals) {
    if (residual.obligation.view() == obligation) {
      return &residual;
    }
  }
  return nullptr;
}


} // namespace repfix

#endif // REP_TESTS_PLANNER_FIXTURE_HPP
