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

struct bounce_source {
	char *file;
	gs_image_file4_t image;
	bool image_initialized;
	uint32_t image_width;
	uint32_t image_height;
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

static void update_draw_size(struct bounce_source *context)
{
	const float scale = (float)context->scale_percent / 100.0f;
	context->draw_width = (uint32_t)fmaxf(1.0f, roundf((float)context->image_width * scale));
	context->draw_height = (uint32_t)fmaxf(1.0f, roundf((float)context->image_height * scale));
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
	const float new_speed = fminf(1200.0f, fmaxf(0.0f, speed));
	const int new_angle = angle < 0 ? 0 : angle > 359 ? 359 : angle;
	const int new_scale_percent = scale_percent < 10 ? 10 : scale_percent > 400 ? 400 : scale_percent;
	const bool speed_changed = context->speed != new_speed;
	const bool angle_changed = context->angle != new_angle;
	const bool scale_changed = context->scale_percent != new_scale_percent;

	context->speed = new_speed;
	context->angle = new_angle;
	context->scale_percent = new_scale_percent;
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

		if (context->image_initialized) {
			obs_enter_graphics();
			gs_image_file4_free(&context->image);
			obs_leave_graphics();
			context->image_initialized = false;
		}

		if (*context->file) {
			gs_image_file4_init(&context->image, context->file, GS_IMAGE_ALPHA_PREMULTIPLY);
			context->image_initialized = true;

			obs_enter_graphics();
			gs_image_file4_init_texture(&context->image);
			obs_leave_graphics();

			struct gs_image_file *image = &context->image.image3.image2.image;
			if (image->loaded) {
				context->image_width = image->cx;
				context->image_height = image->cy;
			} else {
				blog(LOG_WARNING, "DVD Bounce Image could not load '%s'", context->file);
			}
		}
	}

	update_draw_size(context);
	if (file_changed || scale_changed)
		center_image(context);
}

static void bounce_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "file", "");
	obs_data_set_default_double(settings, "speed", DEFAULT_SPEED);
	obs_data_set_default_int(settings, "angle", DEFAULT_ANGLE);
	obs_data_set_default_int(settings, "scale", DEFAULT_SCALE);
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
	if (!image->loaded || !image->texture || !effect)
		return;

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), image->texture);
	gs_matrix_push();
	gs_matrix_translate3f(context->x, context->y, 0.0f);
	gs_draw_sprite(image->texture, 0, context->draw_width, context->draw_height);
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