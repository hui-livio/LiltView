liltview_require_imgui()
liltview_require_spdlog()

# Header/link integration point. Real UI sources await window/render selection.
add_library(liltview_ui INTERFACE)
add_library(LiltView::ui ALIAS liltview_ui)
target_link_libraries(liltview_ui INTERFACE LiltView::core LiltView::imgui)
target_compile_features(liltview_ui INTERFACE cxx_std_${LILTVIEW_CXX_STANDARD})

add_library(liltview_logging STATIC logging/core_log.cpp)
add_library(LiltView::logging ALIAS liltview_logging)
target_include_directories(liltview_logging PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(liltview_logging PUBLIC LiltView::core PRIVATE spdlog::spdlog)
liltview_configure_cxx_target(liltview_logging)

# No app executable until a platform/render backend is chosen.
