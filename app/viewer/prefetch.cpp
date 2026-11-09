#include "prefetch.hpp"

#include <algorithm>
#include <utility>

namespace liltview
{
PrefetchController::PrefetchController(Submit submit, Cancel cancel) :
    submit_(std::move(submit)),
    cancel_(std::move(cancel))
{
}

void PrefetchController::cancel(lv_request_id id) noexcept
{
    try
    {
        if (cancel_)
            cancel_(id);
    } catch (...)
    {
        // A background cancellation diagnostic must not become a foreground image error.
    }
}

void PrefetchController::retire()
{
    for (auto &slot : slots_)
        if (slot.id && !slot.retired)
        {
            slot.retired = true;
            cancel(slot.id);
        }
}

void PrefetchController::update(std::uint64_t generation, const DirectoryNavigation::Snapshot &snapshot)
{
    if (closing_)
        return;
    std::array<std::filesystem::path, 2> paths;
    std::size_t count = 0;
    const auto add    = [&](bool enabled, const std::optional<std::filesystem::path> &path)
    {
        if (!enabled || !path || path->empty() || *path == snapshot.selected_path)
            return;
        if (count && paths[0] == *path)
            return;
        paths[count++] = *path;
    };
    if (!snapshot.selected_path.empty() && !snapshot.scanning && !snapshot.error)
    {
        add(snapshot.can_previous, snapshot.previous_path);
        add(snapshot.can_next, snapshot.next_path);
    }
    bool same = generation == generation_ && selected_ == snapshot.selected_path && count == count_;
    for (std::size_t i = 0; same && i < count; ++i)
        same = targets_[i].path == paths[i];
    if (same)
        return;
    retire();
    ++plan_;
    generation_ = generation;
    selected_   = snapshot.selected_path;
    targets_    = {};
    count_      = count;
    for (std::size_t i = 0; i < count_; ++i)
        targets_[i].path = std::move(paths[i]);
}

void PrefetchController::pump(bool foreground_idle)
{
    const bool quiet_edge = foreground_idle && !foreground_idle_;
    foreground_idle_      = foreground_idle;
    if (closing_ || !foreground_idle)
        return;
    if (quiet_edge)
        for (std::size_t i = 0; i < count_; ++i)
        {
            auto &target = targets_[i];
            if (target.retryable && target.accepted_attempts < 3)
            {
                target.attempted = false;
                target.retryable = false;
            }
        }
    for (std::size_t i = 0; i < count_; ++i)
    {
        auto &target = targets_[i];
        if (target.attempted)
            continue;
        const auto free = std::find_if(slots_.begin(), slots_.end(), [](const Slot &slot) { return !slot.id; });
        if (free == slots_.end())
            break;
        // Prepare owned bookkeeping before the callback can accept an asynchronous request.
        Slot slot;
        slot.path        = target.path;
        slot.plan        = plan_;
        slot.target      = i;
        lv_status status = LV_UNSUPPORTED;
        try
        {
            if (submit_)
                status = submit_(target.path, generation_, slot.id);
        } catch (...)
        {
            status = LV_OUT_OF_MEMORY;
        }
        if (status == LV_BUSY && !slot.id)
            break;
        target.attempted = true;
        if (status != LV_OK || !slot.id)
        {
            ++stats_.failed;
            // Even a broken callback which reports failure after assigning an ID
            // cannot escape the bounded ownership/terminal cleanup path.
            if (slot.id)
            {
                slot.retired = true;
                *free        = std::move(slot);
                ++stats_.submitted;
                cancel(free->id);
            }
            continue;
        }
        *free = std::move(slot);
        ++target.accepted_attempts;
        ++stats_.submitted;
    }
}

bool PrefetchController::owns(lv_request_id id) const noexcept
{
    return id && std::any_of(slots_.begin(), slots_.end(), [id](const Slot &slot) { return slot.id == id; });
}

bool PrefetchController::current(const Slot &slot) const
{
    return !slot.retired && slot.plan == plan_ && slot.target < count_ && targets_[slot.target].path == slot.path;
}

bool PrefetchController::consume(const lv_event_view &event)
{
    const auto found =
        std::find_if(slots_.begin(), slots_.end(), [&event](const Slot &slot) { return slot.id && slot.id == event.request_id; });
    if (found == slots_.end())
        return false;
    if (event.type == LV_EVENT_IMAGE_INFO && current(*found))
    {
        found->info                      = event.as.image;
        targets_[found->target].observed = event.as.image.format;
    }
    if (event.type == LV_EVENT_COMPLETE)
    {
        ++stats_.completed;
        if (event.as.completion.source_bytes_read == 0)
            ++stats_.cache_hits;
        if (current(*found))
            targets_[found->target].info = found->info;
    }
    else if (event.type == LV_EVENT_ERROR)
        ++stats_.failed;
    else if (event.type == LV_EVENT_CANCELLED)
    {
        ++stats_.cancelled;
        if (current(*found))
        {
            auto &target     = targets_[found->target];
            target.retryable = target.accepted_attempts < 3;
        }
    }
    else
        return true; // Metadata or unexpected pixel events remain owned/released by the caller.
    *found = {};
    return true;
}

void PrefetchController::begin_shutdown()
{
    if (closing_)
        return;
    closing_ = true;
    retire();
    ++plan_;
    targets_ = {};
    count_   = 0;
}

bool PrefetchController::pending() const noexcept
{
    if (std::any_of(slots_.begin(), slots_.end(), [](const Slot &slot) { return slot.id != 0; }))
        return true;
    return !closing_ &&
        std::any_of(targets_.begin(), targets_.begin() + count_,
                    [](const Target &target) { return !target.attempted || target.retryable; });
}

PrefetchController::Stats PrefetchController::stats() const noexcept
{
    auto result = stats_;
    for (const auto &slot : slots_)
        if (slot.id)
        {
            ++result.active;
            if (slot.retired)
                ++result.retired;
        }
    return result;
}

std::optional<lv_image_info> PrefetchController::prefetched_info(const std::filesystem::path &path) const
{
    if (!closing_)
        for (std::size_t i = 0; i < count_; ++i)
            if (targets_[i].path == path)
                return targets_[i].info;
    return std::nullopt;
}

std::optional<lv_format> PrefetchController::observed_format(const std::filesystem::path &path) const
{
    if (!closing_)
        for (std::size_t i = 0; i < count_; ++i)
            if (targets_[i].path == path)
                return targets_[i].observed;
    return std::nullopt;
}
} // namespace liltview
