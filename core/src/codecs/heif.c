#include <libheif/heif.h>
#include <libheif/heif_sequences.h>
#include <stdint.h>
#include <string.h>
#include "codec.h"

typedef struct heif_source { lv_core *core; lv_request *request; lv_input *input; lv_error_code failure; } heif_source;
static int64_t heif_pos(void *userdata){ heif_source *s=userdata; long p=ftell(s->input->file); return p < 0 ? -1 : p; }
static int heif_read(void *data,size_t size,void *userdata){ heif_source*s=userdata; if(lv_stopped(s->core,s->request)||size>UINT64_MAX-s->input->version.bytes) return 1; size_t got=fread(data,1,size,s->input->file); s->request->bytes_read+=got; if(got!=size){s->failure=ferror(s->input->file)?LV_ERROR_IO:LV_ERROR_CORRUPT_DATA; return 1;} return 0; }
static int heif_seek(int64_t pos,void*userdata){ heif_source*s=userdata; if(pos<0||(uint64_t)pos>s->input->version.bytes||lv_stopped(s->core,s->request)||!lv_file_seek(s->input->file,(uint64_t)pos)){s->failure=LV_ERROR_IO;return 1;} return 0; }
static enum heif_reader_grow_status heif_wait(int64_t target,void*userdata){ heif_source*s=userdata; return target>=0&&(uint64_t)target<=s->input->version.bytes?heif_reader_grow_status_size_reached:heif_reader_grow_status_size_beyond_eof; }
static lv_error_code heif_map_error(heif_error e, lv_error_code failure){ if(failure)return failure; switch(e.code){case heif_error_Memory_allocation_error:return LV_ERROR_OUT_OF_MEMORY;case heif_error_Unsupported_filetype:case heif_error_Unsupported_feature:return LV_ERROR_FORMAT_UNSUPPORTED;case heif_error_Invalid_input:return LV_ERROR_CORRUPT_DATA;default:return LV_ERROR_CORRUPT_DATA;} }
void lv_decode_heif(lv_core*core,lv_request*request,lv_input*input,lv_error_info*error){
 heif_source source={core,request,input,0}; heif_context*ctx=nullptr; heif_image_handle*handle=nullptr; heif_image*image=nullptr; lv_frame frame={.core=core,.request=request,.input=input};
 heif_reader reader={0}; reader.reader_api_version=1; reader.get_position=heif_pos; reader.read=heif_read; reader.seek=heif_seek; reader.wait_for_file_size=heif_wait;
 heif_error e; ctx=heif_context_alloc(); if(!ctx){error->code=LV_ERROR_OUT_OF_MEMORY;goto done;}
 e=heif_context_read_from_reader(ctx,&reader,&source,nullptr); if(e.code){error->code=heif_map_error(e,source.failure);goto done;}
 if(lv_stopped(core,request))goto done; if(heif_context_has_sequence(ctx)||heif_context_get_number_of_top_level_images(ctx)!=1){error->code=LV_ERROR_FORMAT_UNSUPPORTED;goto done;}
 e=heif_context_get_primary_image_handle(ctx,&handle); if(e.code||!handle){error->code=heif_map_error(e,source.failure);goto done;}
 if(!heif_image_handle_is_primary_image(handle)||heif_image_handle_get_luma_bits_per_pixel(handle)!=8||heif_image_handle_get_number_of_thumbnails(handle)>0||heif_image_handle_has_depth_image(handle)||heif_image_handle_get_number_of_auxiliary_images(handle,0)>0||heif_image_handle_get_color_profile_type(handle)!=heif_color_profile_type_not_present){error->code=LV_ERROR_FORMAT_UNSUPPORTED;goto done;}
 int hinted_w=heif_image_handle_get_width(handle),hinted_h=heif_image_handle_get_height(handle); if(hinted_w<=0||hinted_h<=0){error->code=LV_ERROR_FORMAT_UNSUPPORTED;goto done;}
 e=heif_decode_image(handle,&image,heif_colorspace_RGB,heif_chroma_interleaved_RGBA,nullptr); if(e.code||!image){error->code=heif_map_error(e,source.failure);goto done;}
 int w=heif_image_get_width(image,heif_channel_interleaved),h=heif_image_get_height(image,heif_channel_interleaved); size_t stride=0; const uint8_t*p=heif_image_get_plane_readonly2(image,heif_channel_interleaved,&stride); if(w<=0||h<=0||!p||stride<(size_t)w*4){error->code=LV_ERROR_CORRUPT_DATA;goto done;}
 lv_size_u32 src={(uint32_t)w,(uint32_t)h}; if(!lv_bitmap_size(core,src.width,src.height,error))goto done; lv_size_u32 canvas=request->desc.target.kind==LV_TARGET_NATIVE?src:lv_fit_size(src,request->desc.target.as.fit_bounds,LV_ORIENTATION_NORMAL); uint64_t scratch=(uint64_t)src.width*src.height*4;
 if(!lv_frame_prepare(&frame,src,canvas,LV_FORMAT_HEIF,0,scratch,error)||!lv_frame_info(&frame,error))goto done;
 for(uint32_t y=0;y<canvas.height;y++){uint32_t sy=(uint64_t)y*src.height/canvas.height;uint8_t*out=frame.data+(size_t)y*canvas.width*4;const uint8_t*in=p+(size_t)sy*stride;for(uint32_t x=0;x<canvas.width;x++){uint32_t sx=(uint64_t)x*src.width/canvas.width;memcpy(out+(size_t)x*4,in+(size_t)sx*4,4);}}
 if(!lv_frame_finish(&frame,error))goto done; error->code=0;
done: if(lv_stopped(core,request))error->code=0; if(image)heif_image_release(image); if(handle)heif_image_handle_release(handle); if(ctx)heif_context_free(ctx); lv_frame_release(&frame);
}
