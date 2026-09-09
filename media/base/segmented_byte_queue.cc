// Copyright 2026 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "media/base/segmented_byte_queue.h"

#include <algorithm>
#include <utility>

#include "base/check_op.h"
#include "base/logging.h"
#include "base/notreached.h"

namespace media {

SegmentedByteQueue::SegmentedByteQueue() = default;
SegmentedByteQueue::~SegmentedByteQueue() = default;

void SegmentedByteQueue::Reset() {
  // ~Segment destroys `data` before running `release`, so no span outlives the
  // memory it points at. See the declaration order note in the header.
  segments_.clear();
  total_bytes_ = 0u;
}

void SegmentedByteQueue::Push(base::span<const uint8_t> data,
                              base::ScopedClosureRunner release) {
  if (data.empty()) {
    // Nothing to track, so there is nothing to keep alive either. `release`
    // runs as it goes out of scope here.
    return;
  }

  segments_.push_back(Segment{std::move(release), data});
  total_bytes_ += data.size();
}

void SegmentedByteQueue::Pop(size_t count) {
  CHECK_LE(count, total_bytes_);
  total_bytes_ -= count;

  // Walk forward over the segments this pop consumes entirely, trimming the
  // first partially consumed one in place.
  size_t fully_consumed = 0u;
  while (count > 0u) {
    DCHECK_LT(fully_consumed, segments_.size());
    Segment& front = segments_[fully_consumed];
    if (count < front.data.size()) {
      front.data = front.data.subspan(count);
      break;
    }
    count -= front.data.size();

    // Clear the span here rather than relying on ~Segment. The erase() below
    // move-assigns the surviving segments down over these ones instead of
    // destroying them in place, and member-wise move-assignment runs in
    // declaration order: `release` is assigned first, which runs the old
    // closure and frees the memory, while `data` still points at it. Dropping
    // the span up front avoids that transient dangling reference.
    front.data = {};
    ++fully_consumed;
  }

  segments_.erase(segments_.begin(), segments_.begin() + fully_consumed);
}


base::span<const uint8_t> SegmentedByteQueue::GetContiguousData(
    size_t offset) const {
  // Checked before Locate(), which requires a strictly in-range offset. This
  // also covers an empty queue and a request at the very end of one.
  if (offset >= total_bytes_) {
    return {};
  }

  auto [index, offset_in_segment] = Locate(offset);
  return segments_[index].data.subspan(offset_in_segment);
}

bool SegmentedByteQueue::GetSegmentedData(
    std::vector<base::span<const uint8_t>>* segments,
    size_t size,
    size_t offset) const {
  DCHECK(segments);

  if (offset > total_bytes_ || total_bytes_ - offset < size) {
    return false;
  }

  segments->clear();
  if (size == 0u) {
    return true;
  }

  auto [index, offset_in_segment] = Locate(offset);
  size_t remaining = size;
  while (remaining > 0u) {
    const base::span<const uint8_t> segment_data = segments_[index].data;
    const size_t taken = std::min(remaining, segment_data.size() - offset_in_segment);
    segments->push_back(segment_data.subspan(offset_in_segment, taken));
    remaining -= taken;

    // Only the first segment can be entered part-way through; every later one
    // contributes from its first byte.
    offset_in_segment = 0u;
    ++index;
  }
  return true;
}

bool SegmentedByteQueue::LinearizeData(base::span<uint8_t> dest,
                                       size_t offset) const {
  const size_t size = dest.size();
  if (offset > total_bytes_ || total_bytes_ - offset < size) {
    return false;
  }
  if (size == 0u) {
    return true;
  }

  auto [index, offset_in_segment] = Locate(offset);
  size_t remaining = size;
  while (remaining > 0u) {
    const base::span<const uint8_t> segment_data = segments_[index].data;
    const size_t taken =
        std::min(remaining, segment_data.size() - offset_in_segment);

    // The segments are borrowed input and `dest` is caller-owned output, so
    // the two cannot overlap.
    dest.first(taken).copy_from_nonoverlapping(
        segment_data.subspan(offset_in_segment, taken));
    dest = dest.subspan(taken);
    remaining -= taken;

    // Only the first segment can be entered part-way through; every later one
    // contributes from its first byte.
    offset_in_segment = 0u;
    ++index;
  }
  return true;
}

std::pair<size_t, size_t> SegmentedByteQueue::Locate(size_t offset) const {
  DCHECK_LT(offset, total_bytes_);

  // The front segment is kept trimmed to the unread portion, so no adjustment
  // is needed here.
  for (size_t index = 0u; index < segments_.size(); ++index) {
    const size_t segment_size = segments_[index].data.size();
    if (offset < segment_size) {
      return {index, offset};
    }
    offset -= segment_size;
  }

  // total_bytes_ is kept in sync with the segments, so the offset must have
  // been found above.
  NOTREACHED();
}

}  // namespace media
