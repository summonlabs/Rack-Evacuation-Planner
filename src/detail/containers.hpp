// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal helpers for canonical containers. Not installed.

#ifndef REP_SRC_DETAIL_CONTAINERS_HPP
#define REP_SRC_DETAIL_CONTAINERS_HPP

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rep/status.hpp"

namespace rep::detail {

// Sorts into canonical order and refuses duplicates that are equal in every
// field.  Used for set-like inputs.
template <class T>
[[nodiscard]] Result<std::vector<T>> sorted_unique(std::vector<T> items, std::string_view what) {
  std::sort(items.begin(), items.end());
  for (std::size_t i = 1; i < items.size(); ++i) {
    if (items[i] == items[i - 1]) {
      return make_error(ErrorCode::DuplicateIdentity,
                        std::string(what) + " contains a duplicate entry", std::string(what));
    }
  }
  return items;
}

// Sorts by a key and refuses two records that claim the same key, even when
// their other fields differ.  Used for keyed records, where a repeat is a
// contradiction rather than a repetition.
template <class T, class KeyFn>
[[nodiscard]] Result<std::vector<T>> sorted_unique_by(std::vector<T> items, KeyFn key,
                                                      std::string_view what) {
  std::sort(items.begin(), items.end(),
            [&key](const T& a, const T& b) { return key(a) < key(b); });
  for (std::size_t i = 1; i < items.size(); ++i) {
    const bool ascending = key(items[i - 1]) < key(items[i]);
    const bool descending = key(items[i]) < key(items[i - 1]);
    if (!ascending && !descending) {
      return make_error(ErrorCode::DuplicateIdentity,
                        std::string(what) + " defines " + std::string(what) + " twice",
                        std::string(what));
    }
  }
  return items;
}

// Sorts and removes exact duplicates; used for set-like inputs where a repeat
// is meaningless rather than contradictory.
template <class T>
[[nodiscard]] std::vector<T> sorted_deduped(std::vector<T> items) {
  std::sort(items.begin(), items.end());
  items.erase(std::unique(items.begin(), items.end()), items.end());
  return items;
}

} // namespace rep::detail

#endif // REP_SRC_DETAIL_CONTAINERS_HPP
