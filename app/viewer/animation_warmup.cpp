#include "animation_warmup.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace liltview
{
PreparedAnimationTile::PreparedAnimationTile(const lv_region_info &value) noexcept :
    region(value)
{
    lv_pixels_retain(region.pixels);
}
PreparedAnimationTile::~PreparedAnimationTile()
{
    lv_pixels_release(region.pixels);
}
PreparedAnimationTile::PreparedAnimationTile(PreparedAnimationTile &&other) noexcept :
    region(other.region)
{
    other.region.pixels = nullptr;
}
PreparedAnimationTile &PreparedAnimationTile::operator=(PreparedAnimationTile &&other) noexcept
{
    if (this != &other)
    {
        lv_pixels_release(region.pixels);
        region              = other.region;
        other.region.pixels = nullptr;
    }
    return *this;
}
PreparedAnimation::~PreparedAnimation()
{
    if (core_ && session)
        lv_core_close_animation(core_, session);
}
PreparedAnimation::PreparedAnimation(PreparedAnimation &&other) noexcept :
    session(std::exchange(other.session, 0)),
    info(other.info),
    first(std::move(other.first)),
    second(std::move(other.second)),
    ended(other.ended),
    core_(other.core_)
{
}
PreparedAnimation &PreparedAnimation::operator=(PreparedAnimation &&other) noexcept
{
    if (this != &other)
    {
        if (core_ && session)
            lv_core_close_animation(core_, session);
        session = std::exchange(other.session, 0);
        info    = other.info;
        first   = std::move(other.first);
        second  = std::move(other.second);
        ended   = other.ended;
        core_   = other.core_;
    }
    return *this;
}
lv_animation_id PreparedAnimation::release_session() noexcept
{
    return std::exchange(session, 0);
}

struct AnimationWarmup::State
{
    static constexpr std::uint64_t opaque_header_allowance = 256;
    enum class Stage
    {
        opening,
        first_pending,
        first,
        ready,
        verify_pending,
        verifying,
        retiring
    };
    struct Desired
    {
        std::filesystem::path path;
        bool attempted             = false;
        unsigned accepted_attempts = 0;
    };
    struct Node
    {
        std::filesystem::path path;
        lv_size_u32 bounds{};
        std::uint64_t generation = 0, tag = 0, job_tag = 0, base_bill = 0, first_bill = 0, second_bill = 0;
        lv_animation_id session = 0;
        lv_request_id request   = 0;
        Stage stage             = Stage::opening;
        bool terminal = false, info_received = false, frame_started = false, ended = false, adopting = false, failed = false;
        lv_animation_info info{};
        PreparedAnimationFrame first;
        std::optional<PreparedAnimationFrame> second;
        std::vector<bool> first_seen, second_seen;
    };

    lv_core *core;
    std::uint64_t budget, retained = 0, generation = 0, failure_tag = 0, result_tag = 0, result_bill = 0;
    lv_size_u32 bounds{}, result_bounds{};
    bool closing = false, failure = false;
    std::vector<Desired> desired;
    std::vector<std::unique_ptr<Node>> nodes;
    std::optional<PreparedAnimation> result;
    std::uint64_t completed = 0, adopted = 0, failed = 0;

    State(lv_core *value, std::uint64_t limit) :
        core(value),
        budget(limit)
    {
        if (!core)
            throw std::invalid_argument("Animation warmup requires a Core");
        desired.reserve(2);
        nodes.reserve(4); // Two live jobs plus two retiring jobs, never an unbounded history.
    }

    static bool same(lv_size_u32 a, lv_size_u32 b) noexcept
    {
        return a.width == b.width && a.height == b.height;
    }
    std::size_t outstanding() const noexcept
    {
        return static_cast<std::size_t>(
            std::count_if(nodes.begin(), nodes.end(), [](const auto &node) { return node->request != 0; }));
    }
    static bool add(std::uint64_t &sum, std::uint64_t value, std::uint64_t limit) noexcept
    {
        if (value > limit - sum)
            return false;
        sum += value;
        return true;
    }
    void drop_frames(Node &node)
    {
        retained        -= node.first_bill + node.second_bill;
        node.first_bill = node.second_bill = 0;
        node.first                         = {};
        node.second.reset();
        std::vector<bool>().swap(node.first_seen);
        std::vector<bool>().swap(node.second_seen);
    }
    void retire(Node &node, bool retry = false)
    {
        if (node.stage != Stage::retiring)
        {
            if (node.request && !node.terminal)
                lv_core_cancel(core, node.request);
            if (node.session)
                lv_core_close_animation(core, std::exchange(node.session, 0));
            drop_frames(node);
            node.stage = Stage::retiring;
        }
        if (retry)
            for (auto &item : desired)
                if (item.path == node.path)
                    item.attempted = item.accepted_attempts >= 3;
    }
    void reject(Node &node)
    {
        if (!node.failed)
        {
            node.failed = true;
            ++failed;
        }
        if (node.adopting)
        {
            failure     = true;
            failure_tag = node.tag;
        }
        retire(node);
    }
    void interrupt()
    {
        for (auto &node : nodes)
            if (!node->adopting && node->stage != Stage::ready && node->stage != Stage::retiring)
                retire(*node, true);
    }
    void erase_retired()
    {
        std::erase_if(nodes,
                      [this](const auto &node)
                      {
                          if (node->stage != Stage::retiring || node->request)
                              return false;
                          retained -= node->base_bill;
                          return true;
                      });
    }
    bool frame_complete(const Node &node, bool second) const
    {
        const auto &seen  = second ? node.second_seen : node.first_seen;
        const auto *frame = second ? (node.second ? &*node.second : nullptr) : &node.first;
        return node.frame_started && frame && !seen.empty() && frame->tiles.size() == seen.size() &&
            std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
    }
    bool start_frame(Node &node, const lv_animation_frame_info &frame)
    {
        const bool second = node.stage == Stage::verifying;
        const auto &image = node.info.image;
        if (node.frame_started || frame.key.animation_id != node.session || frame.key.epoch != node.info.epoch ||
            frame.key.frame_index != (second ? 1u : 0u) || !image.canvas.width || !image.canvas.height || !image.tile_edge ||
            image.region.x || image.region.y || !same(image.canvas, {image.region.width, image.region.height}))
            return false;
        const std::uint64_t width = image.canvas.width, height = image.canvas.height, edge = image.tile_edge;
        const std::uint64_t count = ((width + edge - 1) / edge) * ((height + edge - 1) / edge);
        const auto available      = budget - retained;
        std::uint64_t bill        = 0;
        // Core may reuse a pixel allocation up to 25% larger than requested.
        // Reserve ceil(1.25 * (whole backing + opaque header)) from the first
        // partial tile, plus slice/app/coverage descriptors. Never mistake a
        // view's active area for the physical parent allocation it pins.
        constexpr auto tile_overhead = sizeof(PreparedAnimationTile) + opaque_header_allowance + 1;
        if (width > available / 4 / height || count > available / tile_overhead ||
            count > std::numeric_limits<std::size_t>::max())
            return false;
        std::uint64_t backing = 0;
        if (!add(backing, width * height * 4, available) || !add(backing, opaque_header_allowance, available))
            return false;
        const auto pooled_slack = backing / 4 + (backing % 4 != 0);
        if (!add(bill, backing, available) || !add(bill, pooled_slack, available) || !add(bill, count * tile_overhead, available))
            return false;
        if (second)
        {
            node.second_bill = bill;
            node.second.emplace();
        }
        else
            node.first_bill = bill;
        retained     += bill;
        auto &output  = second ? *node.second : node.first;
        auto &seen    = second ? node.second_seen : node.first_seen;
        output.info   = frame;
        output.tiles.reserve(static_cast<std::size_t>(count));
        seen.assign(static_cast<std::size_t>(count), false);
        node.frame_started = true;
        return true;
    }
    bool tile(Node &node, const lv_animation_region_info &value)
    {
        const bool second = node.stage == Stage::verifying;
        auto *frame       = second ? (node.second ? &*node.second : nullptr) : &node.first;
        auto &seen        = second ? node.second_seen : node.first_seen;
        if (!node.frame_started || !frame || value.key.animation_id != frame->info.key.animation_id ||
            value.key.epoch != frame->info.key.epoch || value.key.frame_index != frame->info.key.frame_index)
            return false;
        const auto &region          = value.region;
        const auto *pixels          = lv_pixels_get_view(region.pixels);
        const auto &image           = node.info.image;
        const std::uint64_t columns = (static_cast<std::uint64_t>(image.canvas.width) + image.tile_edge - 1) / image.tile_edge;
        if (region.tile_id >= seen.size() || seen[static_cast<std::size_t>(region.tile_id)] || !pixels || !pixels->data ||
            pixels->format != LV_PIXEL_RGBA8 || region.flags != LV_REGION_FINAL)
            return false;
        const std::uint64_t x = region.tile_id % columns * image.tile_edge;
        const std::uint64_t y = region.tile_id / columns * image.tile_edge;
        const auto width      = std::min<std::uint64_t>(image.tile_edge, image.canvas.width - x);
        const auto height     = std::min<std::uint64_t>(image.tile_edge, image.canvas.height - y);
        if (region.rect.x != x || region.rect.y != y || region.rect.width != width || region.rect.height != height ||
            pixels->size.width != width || pixels->size.height != height || pixels->stride_bytes < width * 4 ||
            (height > 1 && pixels->stride_bytes > (UINT64_MAX - width * 4) / (height - 1)) ||
            pixels->byte_count < (height - 1) * pixels->stride_bytes + width * 4)
            return false;
        frame->tiles.emplace_back(region);
        seen[static_cast<std::size_t>(region.tile_id)] = true;
        return true;
    }
    void finish_events()
    {
        for (auto &node : nodes)
        {
            if (!node->terminal)
                continue;
            node->request  = 0;
            node->terminal = false;
            if (node->stage == Stage::opening)
                node->stage = Stage::first_pending;
            else if (node->stage == Stage::first)
            {
                node->stage = Stage::ready;
                ++completed;
            }
            else if (node->stage == Stage::verifying)
            {
                PreparedAnimation prepared;
                prepared.core_   = core;
                prepared.session = std::exchange(node->session, 0);
                prepared.info    = node->info;
                prepared.first   = std::move(node->first);
                prepared.second  = std::move(node->second);
                prepared.ended   = node->ended;
                result_bill      = node->first_bill + node->second_bill;
                node->first_bill = node->second_bill = 0;
                result_tag                           = node->tag;
                result_bounds                        = node->bounds;
                result.emplace(std::move(prepared));
                node->stage = Stage::retiring;
            }
        }
        erase_retired();
    }
    void submit(Node &node, std::size_t &slots)
    {
        if (!slots || node.request || node.stage == Stage::retiring || node.stage == Stage::ready)
            return;
        lv_status status;
        if (node.stage == Stage::opening)
        {
            const auto owned = std::count_if(nodes.begin(), nodes.end(), [](const auto &value) { return value->session != 0; });
            if (owned + (result && result->session ? 1 : 0) >= 2)
                return;
            lv_animation_open_desc open;
            lv_animation_open_desc_init(&open);
            const auto &path          = node.path.native();
            open.path                 = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
            open.priority             = LV_PRIORITY_VISIBLE_DETAIL;
            open.target.as.fit_bounds = node.bounds;
            open.user_tag             = node.tag;
            status                    = lv_core_open_animation(core, &open, &node.session, &node.request);
        }
        else
        {
            lv_animation_request_desc next;
            lv_animation_request_desc_init(&next);
            next.animation_id = node.session;
            next.priority     = node.adopting ? LV_PRIORITY_CURRENT : LV_PRIORITY_VISIBLE_DETAIL;
            next.user_tag     = node.tag;
            status            = lv_core_submit_animation(core, &next, &node.request);
            if (status == LV_OK)
            {
                node.stage         = node.adopting ? Stage::verifying : Stage::first;
                node.frame_started = false;
            }
        }
        if (status == LV_OK)
        {
            if (node.stage == Stage::opening)
                for (auto &item : desired)
                    if (item.path == node.path)
                        ++item.accepted_attempts;
            node.job_tag = node.tag;
            --slots;
        }
        else if (status != LV_BUSY)
            reject(node);
    }
};

AnimationWarmup::AnimationWarmup(lv_core *core, std::uint64_t retained_budget) :
    state_(std::make_unique<State>(core, retained_budget))
{
}
AnimationWarmup::~AnimationWarmup()
{
    begin_shutdown();
}

void AnimationWarmup::update(std::uint64_t generation, const std::vector<std::filesystem::path> &paths, lv_size_u32 bounds)
{
    auto &s = *state_;
    if (s.closing)
        return;
    std::vector<State::Desired> next;
    next.reserve(2);
    if (s.budget && bounds.width && bounds.height)
        for (const auto &path : paths)
        {
            if (next.size() == 2)
                break;
            if (!path.empty() && path.native().size() <= s.budget &&
                std::none_of(next.begin(), next.end(), [&](const auto &value) { return value.path == path; }))
                next.push_back({path});
        }
    bool changed = s.generation != generation || !State::same(s.bounds, bounds) || s.desired.size() != next.size();
    if (!changed)
        for (std::size_t i = 0; i < next.size(); ++i)
            changed = changed || next[i].path != s.desired[i].path;
    if (changed)
        s.desired = std::move(next);
    s.generation = generation;
    s.bounds     = bounds;
    if (s.failure && s.failure_tag != generation)
        s.failure = false;
    if (s.result && (s.result_tag != generation || !State::same(s.result_bounds, bounds)))
    {
        if (s.result_tag == generation)
        {
            s.failure     = true;
            s.failure_tag = generation;
        }
        s.result.reset();
        s.retained -= std::exchange(s.result_bill, 0);
    }
    for (auto &node : s.nodes)
    {
        if (node->stage == State::Stage::retiring)
            continue;
        if (node->adopting)
        {
            if (node->tag != generation)
                s.retire(*node);
            else if (!State::same(node->bounds, bounds))
                s.reject(*node);
            continue;
        }
        const bool wanted = State::same(node->bounds, bounds) &&
            std::any_of(s.desired.begin(), s.desired.end(), [&](const auto &item) { return item.path == node->path; });
        if (!wanted || (node->stage != State::Stage::ready && node->generation != generation))
            s.retire(*node);
        else
            for (auto &item : s.desired)
                if (item.path == node->path)
                    item.attempted = true;
    }
    s.erase_retired();
}

void AnimationWarmup::pump(bool foreground_idle, std::size_t available_requests)
{
    auto &s = *state_;
    s.finish_events();
    if (s.closing)
        return;
    if (!foreground_idle)
    {
        s.interrupt();
        s.erase_retired();
    }
    auto slots = std::min(available_requests, 2 - std::min<std::size_t>(2, s.outstanding()));
    for (auto &node : s.nodes)
        if (node->adopting && node->stage == State::Stage::verify_pending)
            s.submit(*node, slots);
    if (!foreground_idle || !slots || !s.budget)
        return;
    for (auto &node : s.nodes)
        if (!node->adopting)
            s.submit(*node, slots);
    for (auto &item : s.desired)
    {
        if (!slots || s.nodes.size() >= 4)
            break;
        if (item.attempted)
            continue;
        if (std::any_of(s.nodes.begin(), s.nodes.end(),
                        [&](const auto &node) { return node->stage != State::Stage::retiring && node->path == item.path; }))
            continue;
        item.attempted           = true;
        const std::uint64_t bill = sizeof(State::Node) + item.path.native().size() + 1;
        if (bill > s.budget - s.retained)
        {
            ++s.failed;
            continue;
        }
        auto node        = std::make_unique<State::Node>();
        node->path       = item.path;
        node->bounds     = s.bounds;
        node->generation = node->tag  = s.generation;
        node->base_bill               = bill;
        s.retained                   += bill;
        s.nodes.push_back(std::move(node));
        s.submit(*s.nodes.back(), slots);
    }
    s.erase_retired();
}

bool AnimationWarmup::pending() const noexcept
{
    const auto &s = *state_;
    return s.result.has_value() ||
        std::any_of(s.nodes.begin(), s.nodes.end(), [](const auto &node)
                    { return node->request || (node->stage != State::Stage::ready && node->stage != State::Stage::retiring); }) ||
        (!s.closing && std::any_of(s.desired.begin(), s.desired.end(), [](const auto &item) { return !item.attempted; }));
}

bool AnimationWarmup::owns(lv_request_id id) const noexcept
{
    return id && std::any_of(state_->nodes.begin(), state_->nodes.end(), [id](const auto &node) { return node->request == id; });
}

bool AnimationWarmup::consume(const lv_event_view &view)
{
    auto &s = *state_;
    const auto found =
        std::find_if(s.nodes.begin(), s.nodes.end(), [&](const auto &node) { return node->request == view.request_id; });
    if (!view.request_id || found == s.nodes.end())
        return false;
    auto &node          = **found;
    const bool terminal = view.type == LV_EVENT_COMPLETE || view.type == LV_EVENT_ERROR || view.type == LV_EVENT_CANCELLED;
    if (node.stage == State::Stage::retiring)
    {
        node.terminal = node.terminal || terminal;
        return true;
    }
    try
    {
        bool valid = view.user_tag == node.job_tag;
        if (valid && view.type == LV_EVENT_ANIMATION_INFO)
        {
            valid = node.stage == State::Stage::opening && !node.info_received &&
                view.as.animation.animation_id == node.session && view.as.animation.epoch != 0 &&
                view.as.animation.image.format == LV_FORMAT_GIF;
            if (valid)
            {
                node.info          = view.as.animation;
                node.info_received = true;
            }
        }
        else if (valid && view.type == LV_EVENT_ANIMATION_FRAME_INFO)
            valid = (node.stage == State::Stage::first || node.stage == State::Stage::verifying) &&
                s.start_frame(node, view.as.frame);
        else if (valid && view.type == LV_EVENT_ANIMATION_REGION_READY)
            valid = (node.stage == State::Stage::first || node.stage == State::Stage::verifying) &&
                s.tile(node, view.as.frame_region);
        else if (valid && view.type == LV_EVENT_ANIMATION_END)
        {
            const auto &info = view.as.animation_end.final_info;
            valid = node.stage == State::Stage::verifying && !node.frame_started && info.animation_id == node.session &&
                info.epoch == node.info.epoch && info.image.source_revision == node.info.image.source_revision;
            if (valid)
            {
                node.info  = info;
                node.ended = true;
            }
        }
        else if (view.type == LV_EVENT_COMPLETE)
        {
            valid = valid &&
                (node.stage == State::Stage::opening   ? node.info_received && !view.as.completion.final_tile_count :
                     node.stage == State::Stage::first ? s.frame_complete(node, false) &&
                         view.as.completion.final_tile_count == node.first.tiles.size() :
                                                         node.stage == State::Stage::verifying &&
                         (node.ended ?
                              !view.as.completion.final_tile_count :
                              s.frame_complete(node, true) && view.as.completion.final_tile_count == node.second->tiles.size()));
        }
        else
            valid = false;
        if (!valid)
            s.reject(node);
    } catch (const std::exception &)
    {
        s.reject(node);
    }
    node.terminal = terminal;
    return true;
}

void AnimationWarmup::interrupt_background()
{
    if (!state_->closing)
        state_->interrupt();
}

bool AnimationWarmup::request_adoption(const std::filesystem::path &path, lv_size_u32 bounds, std::uint64_t new_tag)
{
    auto &s = *state_;
    if (s.closing || s.result ||
        std::any_of(s.nodes.begin(), s.nodes.end(),
                    [](const auto &node) { return node->adopting && node->stage != State::Stage::retiring; }))
        return false;
    for (auto &node : s.nodes)
        if (node->stage == State::Stage::ready && node->path == path && State::same(node->bounds, bounds))
        {
            node->adopting      = true;
            node->tag           = new_tag;
            node->stage         = State::Stage::verify_pending;
            node->frame_started = false;
            s.failure           = false;
            s.interrupt();
            return true;
        }
    return false;
}

std::optional<PreparedAnimation> AnimationWarmup::take_adoption()
{
    auto &s = *state_;
    if (!s.result)
        return std::nullopt;
    auto result = std::move(s.result);
    s.result.reset();
    s.retained -= std::exchange(s.result_bill, 0);
    ++s.adopted;
    return result;
}

bool AnimationWarmup::take_adoption_failure() noexcept
{
    return std::exchange(state_->failure, false);
}

void AnimationWarmup::begin_shutdown()
{
    auto &s = *state_;
    if (s.closing)
        return;
    s.closing = true;
    s.failure = false;
    s.desired.clear();
    s.result.reset();
    s.retained -= std::exchange(s.result_bill, 0);
    for (auto &node : s.nodes)
        s.retire(*node);
    s.erase_retired();
}

AnimationWarmup::Stats AnimationWarmup::stats() const noexcept
{
    const auto &s = *state_;
    return {static_cast<std::size_t>(std::count_if(s.nodes.begin(), s.nodes.end(),
                                                   [](const auto &node) { return node->stage == State::Stage::ready; })),
            s.outstanding(),
            s.retained,
            s.completed,
            s.adopted,
            s.failed,
            s.result.has_value() ||
                std::any_of(s.nodes.begin(), s.nodes.end(),
                            [](const auto &node) { return node->adopting && node->stage != State::Stage::retiring; })};
}
} // namespace liltview
