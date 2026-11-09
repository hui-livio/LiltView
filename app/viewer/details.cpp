#include "viewer.hpp"
namespace liltview
{
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
