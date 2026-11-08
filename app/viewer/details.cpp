#include <algorithm>
#include "viewer.hpp"

namespace liltview
{
void Viewer::clear_details()
{
    if (request_ && work_ == Work::static_detail)
    {
        ++generation_;
        lv_core_cancel(core_, request_);
    }
    detail_.clear();
    pending_detail_.clear();
    wanted_detail_    = {};
    submitted_detail_ = {};
    completed_detail_ = {};
    detail_failed_    = false;
}
void Viewer::fit()
{
    viewport_.fit();
    clear_details();
}
void Viewer::actual_size()
{
    viewport_.actual_size();
    queue_details();
}
void Viewer::set_viewport(render::Point min, render::Point max, float density)
{
    viewport_.update(min, max, density, info_.source_size, info_.orientation);
    queue_details();
}
void Viewer::pan(double x, double y)
{
    viewport_.pan(x, y);
    queue_details();
}
void Viewer::zoom(double wheel, render::Point anchor)
{
    viewport_.zoom(wheel, anchor);
    queue_details();
}
void Viewer::queue_details()
{
    if (!display_ || loading_ || playback_.started() || viewport_.fitting() || !(info_.capabilities & LV_CAP_REGION_DECODE))
        return;
    const auto target = viewport_.target(256);
    if (target == wanted_detail_)
        return;
    wanted_detail_ = target;
    detail_failed_ = false;
    ++generation_;
    if (request_ && work_ == Work::static_detail)
        lv_core_cancel(core_, request_);
    pending_detail_.clear();
}
void Viewer::submit_details()
{
    if (request_ || loading_ || animation_id_ || closing_ || viewport_.fitting() || detail_failed_ ||
        !wanted_detail_.rect.width || wanted_detail_ == completed_detail_)
        return;
    const auto path = image_path_.native();
    lv_request_desc request;
    lv_request_desc_init(&request);
    request.path             = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    request.target.kind      = LV_TARGET_REGION;
    request.target.as.region = {wanted_detail_.canvas, wanted_detail_.rect};
    request.user_tag         = generation_;
    request.priority         = LV_PRIORITY_VISIBLE_DETAIL;
    const auto status        = lv_core_submit(core_, &request, &request_);
    if (status == LV_BUSY)
        return;
    if (status != LV_OK)
    {
        detail_failed_ = true;
        error_         = LV_ERROR_INTERNAL;
        return;
    }
    work_                 = Work::static_detail;
    submitted_generation_ = generation_;
    submitted_detail_     = wanted_detail_;
}
void Viewer::consume_details(const lv_event_view &view)
{
    if (view.type == LV_EVENT_IMAGE_INFO)
    {
        const auto &image = view.as.image;
        if (image.source_revision != info_.source_revision || image.source_size.width != info_.source_size.width ||
            image.source_size.height != info_.source_size.height || image.orientation != info_.orientation)
        {
            lv_core_cancel(core_, request_);
            ++generation_;
            detail_failed_ = true;
            error_         = LV_ERROR_SOURCE_CHANGED;
            return;
        }
        pending_detail_.begin(image);
    }
    else if (view.type == LV_EVENT_REGION_READY)
        pending_detail_.upload(view.as.region);
    else if (view.type == LV_EVENT_COMPLETE)
    {
        detail_.swap(pending_detail_);
        pending_detail_.clear();
        completed_detail_ = submitted_detail_;
        error_            = 0;
    }
    else if (view.type == LV_EVENT_ERROR)
    {
        pending_detail_.clear();
        detail_failed_ = true;
        error_         = view.as.error.code;
    }
}
} // namespace liltview
