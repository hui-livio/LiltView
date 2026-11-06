add_library(liltview_core STATIC src/api/config.c)
add_library(LiltView::core ALIAS liltview_core)
target_include_directories(liltview_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
liltview_configure_c_target(liltview_core)

# This source list is explicit. Engine and codec files are added when implemented.
# Do not provide success-returning stubs for the still-unimplemented public API.
