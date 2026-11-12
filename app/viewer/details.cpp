#include "viewer.hpp"
namespace liltview
{
std::optional<ImageDetails> Viewer::image_details() const
{
    if (closing_ || !has_visible_image())
        return std::nullopt;
    const bool reveal = work_ == Work::static_image && loading_ && !pending_scene_.empty();
    ImageDetails details;
    details.path                = reveal ? selected_path_ : displayed_path_;
    details.image               = reveal ? pending_info_ : info_;
    details.pixel_format        = reveal ? pending_pixel_format_ : pixel_format_;
    details.color_space         = reveal ? pending_color_space_ : color_space_;
    details.partial             = reveal;
    details.loading_replacement = loading_ && !reveal;
    details.scale               = !display_ || reveal || !scene_.empty() ? presented_viewport_.scale() : viewport_.scale();
    if (!reveal)
        details.animation = displayed_animation_;
    return details;
}

void Viewer::retry_restore()
{
    if (failed_restore_)
    {
        failed_restore_.reset();
        scene_.retry_reads();
        pending_scene_.retry_reads();
        error_ = 0;
    }
}
// Camera operations are intentionally decode-free, even during a native stream.
void Viewer::fit()
{
    retry_restore();
    viewport_.fit();
}
void Viewer::actual_size()
{
    retry_restore();
    if (can_actual_size())
    {
        viewport_.actual_size();
        if (!scene_.empty() && !loading_)
            error_ = 0; // A failed replacement does not invalidate the cached scene.
    }
}
void Viewer::rotate(int quarter_turns)
{
    retry_restore();
    viewport_.rotate(quarter_turns);
}
void Viewer::set_viewport(render::Point min, render::Point max, float density)
{
    const auto &image = loading_ && !pending_scene_.empty() ? pending_info_ : info_;
    viewport_.update(min, max, density, image.source_size, image.orientation);
}
void Viewer::pan(double x, double y)
{
    retry_restore();
    viewport_.pan(x, y);
}
void Viewer::zoom(double wheel)
{
    retry_restore();
    viewport_.zoom(wheel);
}
void Viewer::zoom(double wheel, render::Point anchor)
{
    retry_restore();
    viewport_.zoom(wheel, anchor);
}
} // namespace liltview
