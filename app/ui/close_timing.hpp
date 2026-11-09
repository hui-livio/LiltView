#ifndef LILTVIEW_CLOSE_TIMING_HPP
#define LILTVIEW_CLOSE_TIMING_HPP

#include <cstdint>
#include <vector>

struct ImGuiContext;

namespace liltview
{
struct CloseTiming
{
    const char *source       = "unknown";
    const char *origin       = "unavailable";
    std::uint64_t event_ns   = 0;
    std::uint64_t handled_ns = 0;
};

// All samples use SDL_GetTicksNS's monotonic clock. -1 means missing/invalid,
// not an instantaneous event; SDL timestamps do not identify a physical click.
inline double close_elapsed_ms(std::uint64_t from, std::uint64_t to)
{
    return from != 0 && to >= from ? static_cast<double>(to - from) / 1000000.0 : -1.0;
}

// Associate SDL input with the exact ImGui EventId consumed by NewFrame(), not
// the latest mouse event (which may still be deferred by input trickling).
class UiCloseTiming
{
public:
    void begin_event(const ImGuiContext &);
    void end_event(const ImGuiContext &, std::uint64_t event_ns, std::uint64_t handled_ns);
    CloseTiming activation(const ImGuiContext &) const;
    void discard_processed(const ImGuiContext &);

private:
    struct Input
    {
        std::uint32_t id;
        CloseTiming timing;
    };
    int first_input_ = 0;
    std::vector<Input> inputs_;
};
} // namespace liltview

#endif
