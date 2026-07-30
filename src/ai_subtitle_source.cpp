#include "ai_subtitle_source.h"
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// Configure cross-platform text engine
#if defined(_WIN32)
#define TEXT_SOURCE_ID "text_gdiplus_v2"
#else
#define TEXT_SOURCE_ID "text_ft2_source"
#endif

// Set text color
static void apply_text_color(obs_data_t *settings, long color)
{
#if defined(_WIN32)
	obs_data_set_int(settings, "color", color);
#else
	obs_data_set_int(settings, "color1", color);
	obs_data_set_int(settings, "color2", color);
	obs_data_set_int(settings, "color", color);
#endif
}

struct MyCaptionsFont {
	obs_source_t *text_font;
	obs_source_t *color_font;
	long cached_bg_color;
};

// Count UTF-8 characters properly
static size_t utf8_char_count(const std::string &s)
{
	size_t count = 0;
	for (char c : s) {
		if ((c & 0xC0) != 0x80)
			count++;
	}
	return count;
}

// Estimate text width for word wrapping
static float estimate_text_width(const std::string &text, int font_size)
{
	const float avg_char_width_ratio = 0.55f;
	return (float)utf8_char_count(text) * (float)font_size * avg_char_width_ratio;
}

// Wrap text by words
static std::string wrap_text_by_words(const std::string &input, int max_width_px, int font_size)
{
	if (max_width_px <= 0 || input.empty())
		return input;

	std::vector<std::string> tokens;
	std::string current_word;
	for (char c : input) {
		if (c == ' ' || c == '\n') {
			if (!current_word.empty()) {
				tokens.push_back(current_word);
				current_word.clear();
			}
			if (c == '\n')
				tokens.push_back("\n");
		} else {
			current_word += c;
		}
	}
	if (!current_word.empty())
		tokens.push_back(current_word);

	std::string result;
	std::string line;
	float line_width = 0.0f;

	for (const std::string &word : tokens) {
		if (word == "\n") {
			result += line;
			result += "\n";
			line.clear();
			line_width = 0.0f;
			continue;
		}

		float word_width = estimate_text_width(word, font_size);
		float space_width = line.empty() ? 0.0f : estimate_text_width(" ", font_size);

		if (!line.empty() && (line_width + space_width + word_width) > (float)max_width_px) {
			result += line;
			result += "\n";
			line.clear();
			line_width = 0.0f;
		}

		if (word_width > (float)max_width_px && line.empty()) {
			std::string current_chunk;
			std::string last_valid_chunk;
			for (char c : word) {
				current_chunk += c;
				if ((c & 0xC0) != 0x80) {
					if (estimate_text_width(current_chunk, font_size) > (float)max_width_px && !last_valid_chunk.empty()) {
						result += last_valid_chunk + "\n";
						current_chunk = current_chunk.substr(last_valid_chunk.length());
					}
					last_valid_chunk = current_chunk;
				}
			}
			line = current_chunk;
			line_width = estimate_text_width(line, font_size);
		} else {
			if (!line.empty()) {
				line += " ";
				line_width += space_width;
			}
			line += word;
			line_width += word_width;
		}
	}
	result += line;

	return result;
}

// Create subtitle source
static void *my_font_create(obs_data_t *settings, obs_source_t *source)
{
	(void)source;
	(void)settings;

	MyCaptionsFont *data = (MyCaptionsFont *)malloc(sizeof(MyCaptionsFont));
	data->cached_bg_color = -1;

	obs_data_t *text_defaults = obs_data_create();
	obs_data_set_string(text_defaults, "text", "Subtítulos IA (Esperando...)");
	apply_text_color(text_defaults, 0xFFFFFFFF);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_string(font_obj, "face", "Arial");
	obs_data_set_string(font_obj, "style", "Regular");
	obs_data_set_int(font_obj, "size", 45);
	obs_data_set_obj(text_defaults, "font", font_obj);
	obs_data_release(font_obj);

	obs_data_t *bg_defaults = obs_data_create();
	obs_data_set_int(bg_defaults, "width", 1);
	obs_data_set_int(bg_defaults, "height", 1);
	obs_data_set_int(bg_defaults, "color", 0x80000000);

	data->text_font = obs_source_create_private(TEXT_SOURCE_ID, "intern_text", text_defaults);
	data->color_font = obs_source_create_private("color_source", "intern_color", bg_defaults);

	obs_data_release(text_defaults);
	obs_data_release(bg_defaults);

	return data;
}

// Destroy subtitle source
static void my_font_destroy(void *data)
{
	MyCaptionsFont *font_data = (MyCaptionsFont *)data;
	obs_source_release(font_data->text_font);
	obs_source_release(font_data->color_font);
	free(font_data);
}

// Render subtitle source
static void my_font_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	MyCaptionsFont *ctx = (MyCaptionsFont *)data;

	uint32_t cx = obs_source_get_width(ctx->text_font);
	uint32_t cy = obs_source_get_height(ctx->text_font);

	if (cx > 0 && cy > 0) {
		gs_matrix_push();
		struct vec3 scale;
		vec3_set(&scale, (float)(cx + 20), (float)(cy + 20), 1.0f);
		gs_matrix_scale(&scale);
		obs_source_video_render(ctx->color_font);
		gs_matrix_pop();
	}

	gs_matrix_push();
	struct vec3 offset;
	vec3_set(&offset, 10.0f, 10.0f, 0.0f);
	gs_matrix_translate(&offset);

	obs_source_video_render(ctx->text_font);

	gs_matrix_pop();
}

// Get source dimensions
static uint32_t my_font_get_width(void *data)
{
	MyCaptionsFont *font_data = (MyCaptionsFont *)data;
	return obs_source_get_width(font_data->text_font) + 20;
}

static uint32_t my_font_get_height(void *data)
{
	MyCaptionsFont *font_data = (MyCaptionsFont *)data;
	return obs_source_get_height(font_data->text_font) + 20;
}

static bool on_word_wrap_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	(void)p;
	bool word_wrap = obs_data_get_bool(settings, "word_wrap");
	obs_property_t *custom_width = obs_properties_get(props, "custom_width");
	if (custom_width) {
		obs_property_set_visible(custom_width, word_wrap);
	}
	return true;
}

// Define source properties
static obs_properties_t *my_font_get_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();

	// Font & Colors group
	obs_properties_t *group_appearance = obs_properties_create();
	obs_properties_add_font(group_appearance, "font", "Tipografía:");
	obs_properties_add_color_alpha(group_appearance, "text_color", "Color de Letras:");
	obs_properties_add_bool(group_appearance, "outline", "Contorno de Texto");
	obs_properties_add_color_alpha(group_appearance, "outline_color", "Color del Contorno:");
	obs_properties_add_bool(group_appearance, "drop_shadow", "Sombra Paralela");
	obs_properties_add_color_alpha(group_appearance, "bg_color", "Color de Fondo:");

	obs_properties_add_group(props, "grp_appearance", "1. Estilo de Fuente y Colores",
				 OBS_GROUP_NORMAL, group_appearance);

	// Layout & Wrap group
	obs_properties_t *group_layout = obs_properties_create();
	obs_properties_add_int(group_layout, "max_lines", "Máximo de Renglones:", 1, 10, 1);

	obs_property_t *p_wrap =
		obs_properties_add_bool(group_layout, "word_wrap", "Activar Salto de Línea (Word Wrap)");
	obs_properties_add_int(group_layout, "custom_width", "Ancho Máximo (píxeles):", 100, 4096, 10);

	obs_property_set_modified_callback(p_wrap, on_word_wrap_changed);

	obs_properties_add_group(props, "grp_layout", "2. Formato y Ajuste de Texto",
				 OBS_GROUP_NORMAL, group_layout);

	return props;
}

// Set default settings
static void my_font_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_int(settings, "outline_color", 0xFF000000);
	obs_data_set_default_bool(settings, "drop_shadow", false);
	obs_data_set_default_int(settings, "bg_color", 0x80000000);

	obs_data_set_default_bool(settings, "word_wrap", true);
	obs_data_set_default_int(settings, "custom_width", 900);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_string(font_obj, "face", "Arial");
	obs_data_set_string(font_obj, "style", "Regular");
	obs_data_set_int(font_obj, "size", 45);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	obs_data_set_default_int(settings, "max_lines", 3);
}

// Update source settings
static void my_font_update(void *data, obs_data_t *settings)
{
	MyCaptionsFont *ctx = (MyCaptionsFont *)data;

	obs_data_t *text_settings = obs_data_create();
	long text_color = obs_data_get_int(settings, "text_color");
	apply_text_color(text_settings, text_color);
	
	obs_data_set_bool(text_settings, "outline", obs_data_get_bool(settings, "outline"));
	obs_data_set_int(text_settings, "outline_color", obs_data_get_int(settings, "outline_color"));
	obs_data_set_bool(text_settings, "drop_shadow", obs_data_get_bool(settings, "drop_shadow"));

	bool word_wrap = obs_data_get_bool(settings, "word_wrap");
	long custom_width = obs_data_get_int(settings, "custom_width");
	int max_lines = (int)obs_data_get_int(settings, "max_lines");

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (!font_obj) {
		font_obj = obs_data_get_default_obj(settings, "font");
	}

	int font_size = 45;

	if (font_obj) {
		font_size = (int)obs_data_get_int(font_obj, "size");
		if (font_size <= 0)
			font_size = 45;
		obs_data_set_obj(text_settings, "font", font_obj);
		obs_data_release(font_obj);
	} else {
		obs_data_t *fallback_font = obs_data_create();
		obs_data_set_string(fallback_font, "face", "Arial");
		obs_data_set_string(fallback_font, "style", "Regular");
		obs_data_set_int(fallback_font, "size", 45);
		obs_data_set_obj(text_settings, "font", fallback_font);
		obs_data_release(fallback_font);
	}

	const char *new_text = obs_data_get_string(settings, "text");
	std::string text_to_set = (new_text) ? new_text : "";

	if (word_wrap) {
		text_to_set = wrap_text_by_words(text_to_set, (int)custom_width, font_size);
	}

	if (max_lines > 0) {
		int newlines_found = 0;
		for (int i = (int)text_to_set.length() - 1; i >= 0; --i) {
			if (text_to_set[i] == '\n') {
				newlines_found++;
				if (newlines_found == max_lines) {
					text_to_set = text_to_set.substr(i + 1);
					break;
				}
			}
		}
	}

	obs_data_set_string(text_settings, "text", text_to_set.c_str());
	obs_source_update(ctx->text_font, text_settings);
	obs_data_release(text_settings);

	long bg_color = obs_data_get_int(settings, "bg_color");
	if (bg_color != ctx->cached_bg_color) {
		obs_data_t *bg_settings = obs_data_create();
		obs_data_set_int(bg_settings, "color", bg_color);
		obs_data_set_int(bg_settings, "width", 1);
		obs_data_set_int(bg_settings, "height", 1);
		obs_source_update(ctx->color_font, bg_settings);
		obs_data_release(bg_settings);
		ctx->cached_bg_color = bg_color;
	}
}

// Define source info struct
extern "C" struct obs_source_info get_my_font_info()
{
	struct obs_source_info info = {0};
	info.id = "fuente_subtitulos_ia";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO;

	info.get_name = [](void *) {
		return "Subtítulos IA";
	};

	info.create = my_font_create;
	info.destroy = my_font_destroy;
	info.video_render = my_font_render;

	info.get_properties = my_font_get_properties;
	info.get_defaults = my_font_get_defaults;
	info.update = my_font_update;

	info.get_width = my_font_get_width;
	info.get_height = my_font_get_height;

	return info;
}