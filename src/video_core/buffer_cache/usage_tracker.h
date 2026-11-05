// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"

namespace VideoCommon {

class UsageTracker {
    // PAGE_SHIFT is a macro on FreeBSD
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;
    /// Pages per group: whole groups of a tracked range are one bit (groups_full).
    static constexpr size_t GROUP_SHIFT = 6;
    static constexpr size_t GROUP_PAGES = size_t{1} << GROUP_SHIFT;
public:
    explicit UsageTracker(size_t size) {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
        const size_t num_group_words = ((num_pages + GROUP_PAGES - 1) >> GROUP_SHIFT) / 64 + 1;
        groups_full.resize(num_group_words, 0ULL);
        groups_any.resize(num_group_words, 0ULL);
    }

    void Reset() noexcept {
        std::ranges::fill(pages, 0ULL);
        std::ranges::fill(groups_full, 0ULL);
        std::ranges::fill(groups_any, 0ULL);
        recent.fill({~u64{0}, 0});
    }

    void Track(u64 offset, u64 size) noexcept {
        // Tracking is idempotent until Reset: skip ranges tracked just before. Draws bind the
        // same (often multi-megabyte) storage buffers again and again.
        for (const auto& [recent_offset, recent_size] : recent) {
            if (recent_offset == offset && recent_size == size) {
                return;
            }
        }
        recent[recent_cursor++ % recent.size()] = {offset, size};
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        // Whole pages in between: a bit per whole group, a word per page at the edges.
        size_t i = page + 1;
        for (; i < page_end && (i & (GROUP_PAGES - 1)) != 0; ++i) {
            SetPageFull(i);
        }
        for (; i + GROUP_PAGES <= page_end; i += GROUP_PAGES) {
            const size_t group = i >> GROUP_SHIFT;
            groups_full[group / 64] |= u64{1} << (group % 64);
        }
        for (; i < page_end; ++i) {
            SetPageFull(i);
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return false;
        }
        if (IsPageUsed(page, offset, size)) {
            return true;
        }
        size_t i = page + 1;
        for (; i < page_end && (i & (GROUP_PAGES - 1)) != 0; ++i) {
            if (pages[i] != 0 || GroupFull(i >> GROUP_SHIFT)) {
                return true;
            }
        }
        for (; i + GROUP_PAGES <= page_end; i += GROUP_PAGES) {
            const size_t group = i >> GROUP_SHIFT;
            if (((groups_full[group / 64] | groups_any[group / 64]) >> (group % 64)) & 1) {
                return true;
            }
        }
        for (; i < page_end; ++i) {
            if (pages[i] != 0 || GroupFull(i >> GROUP_SHIFT)) {
                return true;
            }
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        return IsPageUsed(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

private:
    bool GroupFull(size_t group) const noexcept {
        return ((groups_full[group / 64] >> (group % 64)) & 1) != 0;
    }

    void NoteGroupAny(size_t page) noexcept {
        const size_t group = page >> GROUP_SHIFT;
        groups_any[group / 64] |= u64{1} << (group % 64);
    }

    void SetPageFull(size_t page) noexcept {
        pages[page] = ~u64{0};
        NoteGroupAny(page);
    }

    void TrackPage(u64 page, u64 offset, u64 size) noexcept {
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = std::min<size_t>(size, BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t mask = ~u64{0} >> (64 - num_bits);
        pages[page] |= (~u64{0} & mask) << first_bit;
        NoteGroupAny(page);
    }

    bool IsPageUsed(u64 page, u64 offset, u64 size) const noexcept {
        if (GroupFull(page >> GROUP_SHIFT)) {
            return true;
        }
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = std::min<size_t>(size, BUFFER_PAGE_BYTES) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t mask = ~u64{0} >> (64 - num_bits);
        const size_t mask2 = (~u64{0} & mask) << first_bit;
        return (pages[page] & mask2) != 0;
    }

private:
    std::vector<u64> pages;
    /// A bit per group of GROUP_PAGES pages: all of them used / any of their page words set.
    std::vector<u64> groups_full;
    std::vector<u64> groups_any;
    /// Ranges tracked last, (offset, size).
    std::array<std::pair<u64, u64>, 4> recent{{{~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}}};
    size_t recent_cursor{};
};

} // namespace VideoCommon
