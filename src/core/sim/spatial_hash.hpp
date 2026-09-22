#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace core::sim {

// Flat spatial hash grid, allocation-free after warm-up.
// Entities are bucketed by hashed cell with counting sort; queries verify
// the exact cell, so hash collisions produce no false duplicates.
class SpatialHash {
public:
  explicit SpatialHash(float cellSize = 1.0F, std::size_t bucketCount = 16384)
      : cellSize_(cellSize), bucketCount_(bucketCount) {
    starts_.assign(bucketCount_ + 1, 0);
    cellX_.reserve(4096);
    cellY_.reserve(4096);
    items_.reserve(4096);
  }

  void build(const float* xs, const float* ys, const std::uint32_t* ids, std::size_t n) {
    cellX_.resize(n);
    cellY_.resize(n);
    items_.resize(n);

    for (std::size_t i = 0; i < n; ++i) {
      cellX_[i] = static_cast<std::int32_t>(std::floor(xs[i] / cellSize_));
      cellY_[i] = static_cast<std::int32_t>(std::floor(ys[i] / cellSize_));
      items_[i] = ids[i];
    }

    // counting sort by hashed bucket
    for (std::size_t b = 0; b <= bucketCount_; ++b) {
      starts_[b] = 0;
    }
    for (std::size_t i = 0; i < n; ++i) {
      ++starts_[hash(cellX_[i], cellY_[i]) + 1];
    }
    for (std::size_t b = 1; b <= bucketCount_; ++b) {
      starts_[b] += starts_[b - 1];
    }

    cursor_.resize(bucketCount_);
    for (std::size_t b = 0; b < bucketCount_; ++b) {
      cursor_[b] = starts_[b];
    }

    // permute entries into sorted order (sort cell coords + ids in parallel)
    sortedX_.resize(n);
    sortedY_.resize(n);
    sortedItems_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t dst = cursor_[hash(cellX_[i], cellY_[i])]++;
      sortedX_[dst] = cellX_[i];
      sortedY_[dst] = cellY_[i];
      sortedItems_[dst] = items_[i];
    }
    cellX_.swap(sortedX_);
    cellY_.swap(sortedY_);
    items_.swap(sortedItems_);
    count_ = n;
  }

  // Calls fn(entityId) for every entity whose cell intersects the query circle.
  template <class F>
  void forEachNear(float x, float y, float radius, F&& fn) const {
    const auto cx0 = static_cast<std::int32_t>(std::floor((x - radius) / cellSize_));
    const auto cx1 = static_cast<std::int32_t>(std::floor((x + radius) / cellSize_));
    const auto cy0 = static_cast<std::int32_t>(std::floor((y - radius) / cellSize_));
    const auto cy1 = static_cast<std::int32_t>(std::floor((y + radius) / cellSize_));
    for (auto cy = cy0; cy <= cy1; ++cy) {
      for (auto cx = cx0; cx <= cx1; ++cx) {
        const std::size_t b = hash(cx, cy);
        const std::size_t begin = starts_[b];
        const std::size_t end = starts_[b + 1];
        for (std::size_t i = begin; i < end; ++i) {
          if (cellX_[i] == cx && cellY_[i] == cy) {
            fn(items_[i]);
          }
        }
      }
    }
  }

  [[nodiscard]] std::size_t size() const { return count_; }

private:
  [[nodiscard]] std::size_t hash(std::int32_t cx, std::int32_t cy) const {
    const std::uint32_t h = static_cast<std::uint32_t>(cx) * 73856093u ^
                             static_cast<std::uint32_t>(cy) * 19349663u;
    return h & (bucketCount_ - 1); // bucketCount_ is a power of two
  }

  float cellSize_;
  std::size_t bucketCount_;
  std::size_t count_ = 0;

  std::vector<std::size_t> starts_;   // bucketCount+1 offsets
  std::vector<std::size_t> cursor_;   // bucketCount scratch
  std::vector<std::int32_t> cellX_, cellY_;
  std::vector<std::uint32_t> items_;
  std::vector<std::int32_t> sortedX_, sortedY_;
  std::vector<std::uint32_t> sortedItems_;
};

} // namespace core::sim
