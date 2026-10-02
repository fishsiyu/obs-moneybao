#include <math.h>
#include <string.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <util/bmem.h>
#include <util/threading.h>

#define SOURCE_ID "dvd_bounce_image_source"
#define DEFAULT_SPEED 180.0
#define DEFAULT_ANGLE 45
#define DEFAULT_SCALE 100
#define MAX_CROP_PERCENT 49
#define AUTO_CROP_ALPHA_THRESHOLD 8

struct bounce_source {
	char *file;
	gs_image_file4_t image;
	bool image_initialized;
	uint32_t image_width;
	uint32_t image_height;
	/* crop settings, in percent of the source image */
	int crop_left;
	int crop_right;
	int crop_top;
	int crop_bottom;
	bool auto_crop;
	/* bounding box of the non-transparent pixels, detected from the alpha channel */
	bool auto_bounds_valid;
	uint32_t auto_x;
	uint32_t auto_y;
	uint32_t auto_width;
	uint32_t auto_height;
	/* visible (cropped) region inside the source image, in image pixels */
	uint32_t visible_x;
	uint32_t visible_y;
	uint32_t visible_width;
	uint32_t visible_height;
	uint32_t draw_width;
	uint32_t draw_height;
	float x;
	float y;
	float velocity_x;
	float velocity_y;
	float speed;
	float direction_x;
	float direction_y;
	int angle;
	int scale_percent;
	bool direction_initialized;
	volatile bool reset_requested;
	uint64_t last_frame_time;
};

static void bounce_source_update(void *data, obs_data_t *settings);

static void get_canvas_size(uint32_t *width, uint32_t *height)
{
	struct obs_video_info video_info = {0};
	if (obs_get_video_info(&video_info)) {
		*width = video_info.base_width;
		*height = video_info.base_height;
	} else {
		*width = 1920;
		*height = 1080;
	}
}

static void update_direction(struct bounce_source *context)
{
	const float radians = (float)context->angle * 0.017453292519943295f;
	context->direction_x = cosf(radians);
	context->direction_y = sinf(radians);
	context->velocity_x = context->direction_x * context->speed;
	context->velocity_y = context->direction_y * context->speed;
	context->direction_initialized = true;
}

/*
 * Scans the CPU-side pixels of the loaded image and stores the bounding box of
 * every pixel that is not (almost) fully transparent.  This must be called
 * before the texture is created, because OBS frees the raw pixel data
 * afterwards.
 */
static void compute_auto_crop(struct bounce_source *context)
{
	context->auto_bounds_valid = false;
	context->auto_x = 0;
	context->auto_y = 0;
	context->auto_width = context->image_width;
	context->auto_height = context->image_height;

	if (!context->image_initialized || !context->image_width || !context->image_height)
		return;

	struct gs_image_file *image = &context->image.image3.image2.image;
	const uint8_t *data = NULL;
	size_t bytes_per_pixel = 4;
	size_t alpha_index = 3;

	if (image->is_animated_gif) {
		/* libnsgif buffers are always 32bit RGBA */
		data = (const uint8_t *)image->gif.frame_image;
	} else {
		data = image->texture_data;
		switch (image->format) {
		case GS_RGBA:
		case GS_BGRA:
			bytes_per_pixel = 4;
			alpha_index = 3;
			break;
		case GS_A8:
			bytes_per_pixel = 1;
			alpha_index = 0;
			break;
		default:
			/* no usable alpha channel (e.g. BGRX / RGBA16): keep the whole image */
			data = NULL;
			break;
		}
	}

	if (!data)
		return;

	const uint32_t width = context->image_width;
	const uint32_t height = context->image_height;
	uint32_t min_x = width;
	uint32_t min_y = height;
	uint32_t max_x = 0;
	uint32_t max_y = 0;
	bool found = false;

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = data + (size_t)y * width * bytes_per_pixel;
		for (uint32_t x = 0; x < width; x++) {
			if (row[(size_t)x * bytes_per_pixel + alpha_index] > AUTO_CROP_ALPHA_THRESHOLD) {
				if (x < min_x)
					min_x = x;
				if (x > max_x)
					max_x = x;
				if (y < min_y)
					min_y = y;
				if (y > max_y)
					max_y = y;
				found = true;
			}
		}
	}

	if (!found)
		return;

	context->auto_x = min_x;
	context->auto_y = min_y;
	context->auto_width = max_x - min_x + 1;
	context->auto_height = max_y - min_y + 1;
	context->auto_bounds_valid = true;
}

/* Combines the manual crop with the auto-detected transparent border. */
static void update_visible_region(struct bounce_source *context)
{
	const uint32_t width = context->image_width;
	const uint32_t height = context->image_height;
	if (!width || !height) {
		context->visible_x = 0;
		context->visible_y = 0;
		context->visible_width = 0;
		context->visible_height = 0;
		return;
	}

	uint32_t left = (uint32_t)((uint64_t)width * (uint32_t)context->crop_left / 100);
	uint32_t right = (uint32_t)((uint64_t)width * (uint32_t)context->crop_right / 100);
	uint32_t top = (uint32_t)((uint64_t)height * (uint32_t)context->crop_top / 100);
	uint32_t bottom = (uint32_t)((uint64_t)height * (uint32_t)context->crop_bottom / 100);

	if (context->auto_crop && context->auto_bounds_valid) {
		const uint32_t auto_left = context->auto_x;
		const uint32_t auto_top = context->auto_y;
		const uint32_t auto_right = width - (context->auto_x + context->auto_width);
		const uint32_t auto_bottom = height - (context->auto_y + context->auto_height);
		left = left > auto_left ? left : auto_left;
		top = top > auto_top ? top : auto_top;
		right = right > auto_right ? right : auto_right;
		bottom = bottom > auto_bottom ? bottom : auto_bottom;
	}

	/* never crop everything away: keep at least one row/column of pixels */
	if (left + right >= width) {
		if (left >= width - 1)
			left = width - 1;
		right = width - 1 - left;
	}
	if (top + bottom >= height) {
		if (top >= height - 1)
			top = height - 1;
		bottom = height - 1 - top;
	}

	context->visible_x = left;
	context->visible_y = top;
	context->visible_width = width - left - right;
	context->visible_height = height - top - bottom;
}

static void update_draw_size(struct bounce_source *context)
{
	const float scale = (float)context->scale_percent / 100.0f;
	const uint32_t source_width = context->visible_width ? context->visible_width : 1;
	const uint32_t source_height = context->visible_height ? context->visible_height : 1;
	context->draw_width = (uint32_t)fmaxf(1.0f, roundf((float)source_width * scale));
	context->draw_height = (uint32_t)fmaxf(1.0f, roundf((float)source_height * scale));
}

static void center_image(struct bounce_source *context)
{
	uint32_t canvas_width;
	uint32_t canvas_height;
	get_canvas_size(&canvas_width, &canvas_height);
	context->x = fmaxf(0.0f, ((float)canvas_width - (float)context->draw_width) * 0.5f);
	context->y = fmaxf(0.0f, ((float)canvas_height - (float)context->draw_height) * 0.5f);
}

static float advance_axis(float position, float *velocity, float *direction, float speed, float canvas_extent,
			  float image_extent, float seconds)
{
	const float limit = canvas_extent - image_extent;
	if (limit <= 0.0f) {
		*velocity = 0.0f;
		return 0.0f;
	}

	const double period = (double)limit * 2.0;
	const double unfolded = *velocity >= 0.0f ? position : period - position;
	double phase = fmod(unfolded + speed * seconds, period);
	if (phase < 0.0)
		phase += period;

	if (phase < limit) {
		*direction = fabsf(*direction);
		*velocity = *direction * speed;
		return (float)phase;
	}

	*direction = -fabsf(*direction);
	*velocity = *direction * speed;
	return (float)(period - phase);
}

static const char *bounce_source_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SourceName");
}

static void *bounce_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	struct bounce_source *context = bzalloc(sizeof(*context));
	bounce_source_update(context, settings);
	return context;
}

static void bounce_source_destroy(void *data)
{
	struct bounce_source *context = data;
	if (context->image_initialized) {
		obs_enter_graphics();
		gs_image_file4_free(&context->image);
		obs_leave_graphics();
	}
	bfree(context->file);
	bfree(context);
}

static int clamp_crop_percent(int value)
{
	if (value < 0)
		return 0;
	if (value > MAX_CROP_PERCENT)
		return MAX_CROP_PERCENT;
	return value;
}

static void bounce_source_update(void *data, obs_data_t *settings)
{
	struct bounce_source *context = data;
	const char *file = obs_data_get_string(settings, "file");
	if (!file)
		file = "";
	const bool file_changed = !context->file || strcmp(context->file, file) != 0;
	const float speed = (float)obs_data_get_double(settings, "speed");
	const int angle = (int)obs_data_get_int(settings, "angle");
	const int scale_percent = (int)obs_data_get_int(settings, "scale");
	const int crop_left = (int)obs_data_get_int(settings, "crop_left");
	const int crop_right = (int)obs_data_get_int(settings, "crop_right");
	const int crop_top = (int)obs_data_get_int(settings, "crop_top");
	const int crop_bottom = (int)obs_data_get_int(settings, "crop_bottom");
	const bool auto_crop = obs_data_get_bool(settings, "auto_crop");
	const float new_speed = fminf(1200.0f, fmaxf(0.0f, speed));
	const int new_angle = angle < 0 ? 0 : angle > 359 ? 359 : angle;
	const int new_scale_percent = scale_percent < 10 ? 10 : scale_percent > 400 ? 400 : scale_percent;
	const int new_crop_left = clamp_crop_percent(crop_left);
	const int new_crop_right = clamp_crop_percent(crop_right);
	const int new_crop_top = clamp_crop_percent(crop_top);
	const int new_crop_bottom = clamp_crop_percent(crop_bottom);
	const bool speed_changed = context->speed != new_speed;
	const bool angle_changed = context->angle != new_angle;
	const bool scale_changed = context->scale_percent != new_scale_percent;
	const bool crop_changed = context->crop_left != new_crop_left || context->crop_right != new_crop_right ||
				  context->crop_top != new_crop_top || context->crop_bottom != new_crop_bottom;
	const bool auto_crop_changed = context->auto_crop != auto_crop;

	context->speed = new_speed;
	context->angle = new_angle;
	context->scale_percent = new_scale_percent;
	context->crop_left = new_crop_left;
	context->crop_right = new_crop_right;
	context->crop_top = new_crop_top;
	context->crop_bottom = new_crop_bottom;
	context->auto_crop = auto_crop;
	if (!context->direction_initialized || angle_changed) {
		update_direction(context);
	} else if (speed_changed) {
		context->velocity_x = context->direction_x * context->speed;
		context->velocity_y = context->direction_y * context->speed;
	}

	if (file_changed) {
		bfree(context->file);
		context->file = bstrdup(file);
		context->image_width = 0;
		context->image_height = 0;
		context->last_frame_time = 0;
		context->auto_bounds_valid = false;
		context->auto_x = 0;
		context->auto_y = 0;
		context->auto_width = 0;
		context->auto_height = 0;

		if (context->image_initialized) {
			obs_enter_graphics();
			gs_image_file4_free(&context->image);
			obs_leave_graphics();
			context->image_initialized = false;
		}

		if (*context->file) {
			gs_image_file4_init(&context->image, context->file, GS_IMAGE_ALPHA_PREMULTIPLY);
			context->image_initialized = true;

			struct gs_image_file *image = &context->image.image3.image2.image;
			if (image->loaded) {
				context->image_width = image->cx;
				context->image_height = image->cy;
				/* the raw pixels are freed once the texture exists */
				compute_auto_crop(context);
			}

			obs_enter_graphics();
			gs_image_file4_init_texture(&context->image);
			obs_leave_graphics();

			if (!image->loaded)
				blog(LOG_WARNING, "DVD Bounce Image could not load '%s'", context->file);
		}
	}

	update_visible_region(context);
	update_draw_size(context);
	if (file_changed || scale_changed || crop_changed || auto_crop_changed)
		center_image(context);
}

static void bounce_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "file", "");
	obs_data_set_default_double(settings, "speed", DEFAULT_SPEED);
	obs_data_set_default_int(settings, "angle", DEFAULT_ANGLE);
	obs_data_set_default_int(settings, "scale", DEFAULT_SCALE);
	obs_data_set_default_int(settings, "crop_left", 0);
	obs_data_set_default_int(settings, "crop_right", 0);
	obs_data_set_default_int(settings, "crop_top", 0);
	obs_data_set_default_int(settings, "crop_bottom", 0);
	obs_data_set_default_bool(settings, "auto_crop", true);
}

static bool bounce_source_reset_position(obs_properties_t *properties, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);
	struct bounce_source *context = data;
	os_atomic_set_bool(&context->reset_requested, true);
	return false;
}

static obs_properties_t *bounce_source_properties(void *data)
{
	obs_properties_t *properties = obs_properties_create();
	static const char *image_filter = "Image files (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp);;All files (*.*)";

	obs_properties_add_path(properties, "file", obs_module_text("ImageFile"), OBS_PATH_FILE, image_filter, NULL);

	obs_properties_t *crop = obs_properties_create();
	obs_properties_add_int_slider(crop, "crop_left", obs_module_text("CropLeft"), 0, MAX_CROP_PERCENT, 1);
	obs_properties_add_int_slider(crop, "crop_right", obs_module_text("CropRight"), 0, MAX_CROP_PERCENT, 1);
	obs_properties_add_int_slider(crop, "crop_top", obs_module_text("CropTop"), 0, MAX_CROP_PERCENT, 1);
	obs_properties_add_int_slider(crop, "crop_bottom", obs_module_text("CropBottom"), 0, MAX_CROP_PERCENT, 1);
	obs_property_t *auto_crop = obs_properties_add_bool(crop, "auto_crop", obs_module_text("AutoCrop"));
	obs_property_set_long_description(auto_crop, obs_module_text("AutoCropTip"));
	obs_properties_add_group(properties, "crop_group", obs_module_text("CropGroup"), OBS_GROUP_NORMAL, crop);

	obs_properties_add_float_slider(properties, "speed", obs_module_text("Speed"), 0.0, 1200.0, 1.0);
	obs_properties_add_int_slider(properties, "angle", obs_module_text("Angle"), 0, 359, 1);
	obs_properties_add_int_slider(properties, "scale", obs_module_text("Scale"), 10, 400, 5);
	obs_properties_add_button2(properties, "reset_position", obs_module_text("ResetPosition"),
				   bounce_source_reset_position, data);
	return properties;
}

static uint32_t bounce_source_width(void *data)
{
	UNUSED_PARAMETER(data);
	uint32_t width;
	uint32_t height;
	get_canvas_size(&width, &height);
	UNUSED_PARAMETER(height);
	return width;
}

static uint32_t bounce_source_height(void *data)
{
	UNUSED_PARAMETER(data);
	uint32_t width;
	uint32_t height;
	get_canvas_size(&width, &height);
	UNUSED_PARAMETER(width);
	return height;
}

static void bounce_source_tick(void *data, float seconds)
{
	struct bounce_source *context = data;
	uint32_t canvas_width;
	uint32_t canvas_height;
	get_canvas_size(&canvas_width, &canvas_height);

	if (os_atomic_load_bool(&context->reset_requested)) {
		center_image(context);
		os_atomic_set_bool(&context->reset_requested, false);
	}

	context->velocity_x = context->direction_x * context->speed;
	context->velocity_y = context->direction_y * context->speed;
	context->x = advance_axis(context->x, &context->velocity_x, &context->direction_x, context->speed,
				 (float)canvas_width, (float)context->draw_width, seconds);
	context->y = advance_axis(context->y, &context->velocity_y, &context->direction_y, context->speed,
				 (float)canvas_height, (float)context->draw_height, seconds);

	struct gs_image_file *image = &context->image.image3.image2.image;
	if (image->is_animated_gif) {
		const uint64_t frame_time = obs_get_video_frame_time();
		if (context->last_frame_time && frame_time >= context->last_frame_time) {
			const uint64_t elapsed = frame_time - context->last_frame_time;
			if (gs_image_file4_tick(&context->image, elapsed)) {
				obs_enter_graphics();
				gs_image_file4_update_texture(&context->image);
				obs_leave_graphics();
			}
		}
		context->last_frame_time = frame_time;
	}
}

static void bounce_source_render(void *data, gs_effect_t *effect)
{
	struct bounce_source *context = data;
	struct gs_image_file *image = &context->image.image3.image2.image;
	if (!image->loaded || !image->texture || !effect || !context->visible_width || !context->visible_height)
		return;

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), image->texture);
	gs_matrix_push();
	gs_matrix_translate3f(context->x, context->y, 0.0f);
	gs_matrix_scale3f((float)context->draw_width / (float)context->visible_width,
			  (float)context->draw_height / (float)context->visible_height, 1.0f);
	gs_draw_sprite_subregion(image->texture, 0, context->visible_x, context->visible_y, context->visible_width,
				 context->visible_height);
	gs_matrix_pop();
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}

static enum gs_color_space bounce_source_color_space(void *data, size_t count,
						      const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	struct bounce_source *context = data;
	struct gs_image_file *image = &context->image.image3.image2.image;
	return image->texture ? context->image.space : GS_CS_SRGB;
}

struct obs_source_info bounce_source_info = {
	.id = SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = bounce_source_name,
	.create = bounce_source_create,
	.destroy = bounce_source_destroy,
	.update = bounce_source_update,
	.get_defaults = bounce_source_defaults,
	.get_properties = bounce_source_properties,
	.get_width = bounce_source_width,
	.get_height = bounce_source_height,
	.video_tick = bounce_source_tick,
	.video_render = bounce_source_render,
	.video_get_color_space = bounce_source_color_space,
	.icon_type = OBS_ICON_TYPE_IMAGE,
};