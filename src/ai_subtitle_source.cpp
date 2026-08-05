#include "ai_subtitle_source.h"
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// Configure cross-platform text source identifier
#if defined(_WIN32)
#define TEXT_SOURCE_ID "text_gdiplus_v2"
#else
#define TEXT_SOURCE_ID "text_ft2_source"
#endif

// Set text color property
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
	long cached_bg_color{-1};
	
	std::string cached_raw_text;
	std::string cached_text;
	long cached_text_color{-1};
	bool cached_outline{false};
	long cached_outline_color{-1};
	bool cached_drop_shadow{false};
	std::string cached_font_face;
	int cached_font_size{0};
	std::string cached_font_style;
	bool cached_fixed_bg_width{false};
	long cached_custom_width{-1};
	bool cached_bottom_align{false};
	int cached_max_lines{-1};
	// Partial/Final color state
	bool cached_is_partial{false};
	bool cached_word_wrap{true};
};

// Count UTF-8 characters
static size_t utf8_char_count(const std::string &s)
{
	size_t count = 0;
	for (char c : s) {
		if ((c & 0xC0) != 0x80)
			count++;
	}
	return count;
}

// Text width estimation and manual wrap removed in favor of native OBS word wrap.

// Create subtitle source instance
static void *my_font_create(obs_data_t *settings, obs_source_t *source)
{
	(void)source;
	(void)settings;

	MyCaptionsFont *data = new MyCaptionsFont();
	data->cached_bg_color = -1;
	data->cached_fixed_bg_width = false;
	data->cached_custom_width = 900;
	data->cached_bottom_align = false;
	data->cached_max_lines = 3;

	obs_data_t *text_defaults = obs_data_create();
	obs_data_set_string(text_defaults, "text", "");
	apply_text_color(text_defaults, 0xFFFFFFFF);

	// Set initial word wrap and extents properties for both GDI+ (Windows) and FreeType2 (Linux/Mac)
	obs_data_set_bool(text_defaults, "extents", true);
	obs_data_set_bool(text_defaults, "extents_wrap", true);
	obs_data_set_int(text_defaults, "extents_cx", 900);
	obs_data_set_int(text_defaults, "extents_cy", 0);
	obs_data_set_bool(text_defaults, "word_wrap", true);
	obs_data_set_int(text_defaults, "custom_width", 900);
	obs_data_set_int(text_defaults, "cx", 900);

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

// Destroy subtitle source instance
static void my_font_destroy(void *data)
{
	MyCaptionsFont *font_data = (MyCaptionsFont *)data;
	obs_source_release(font_data->text_font);
	obs_source_release(font_data->color_font);
	delete font_data;
}

// Render subtitle source frames
static void my_font_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	MyCaptionsFont *ctx = (MyCaptionsFont *)data;

	uint32_t cx = obs_source_get_width(ctx->text_font);
	uint32_t cy = obs_source_get_height(ctx->text_font);
	
	float bg_width = ctx->cached_fixed_bg_width ? (float)ctx->cached_custom_width : (float)(cx + 20);
	
	// Estimate max height for vertical positioning
	float estimated_line_height = (float)ctx->cached_font_size * 1.4f;
	float max_height = ctx->cached_max_lines * estimated_line_height + 20.0f;
	float bg_height = ctx->cached_bottom_align ? max_height : (float)(cy + 20);

	if (bg_width > 0 && bg_height > 0) {
		gs_matrix_push();
		struct vec3 scale;
		vec3_set(&scale, bg_width, bg_height, 1.0f);
		gs_matrix_scale(&scale);
		obs_source_video_render(ctx->color_font);
		gs_matrix_pop();
	}

	gs_matrix_push();
	struct vec3 offset;
	
	if (ctx->cached_bottom_align) {
		// Align text vertically in fixed background
		float y_offset = (max_height - (float)cy) / 2.0f;
		if (y_offset < 10.0f) y_offset = 10.0f;
		
		vec3_set(&offset, 10.0f, y_offset, 0.0f);
	} else {
		vec3_set(&offset, 10.0f, 10.0f, 0.0f);
	}
	
	gs_matrix_translate(&offset);

	obs_source_video_render(ctx->text_font);

	gs_matrix_pop();
}

// Calculate source dimensions
static uint32_t my_font_get_width(void *data)
{
	MyCaptionsFont *ctx = (MyCaptionsFont *)data;
	if (ctx->cached_fixed_bg_width)
		return ctx->cached_custom_width;
	return obs_source_get_width(ctx->text_font) + 20;
}

static uint32_t my_font_get_height(void *data)
{
	MyCaptionsFont *ctx = (MyCaptionsFont *)data;
	if (ctx->cached_bottom_align) {
		float estimated_line_height = (float)ctx->cached_font_size * 1.4f;
		return (uint32_t)(ctx->cached_max_lines * estimated_line_height + 20.0f);
	}
	return obs_source_get_height(ctx->text_font) + 20;
}

static bool on_word_wrap_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	(void)p;
	bool word_wrap = obs_data_get_bool(settings, "word_wrap");
	obs_property_t *custom_width = obs_properties_get(props, "custom_width");
	obs_property_t *fixed_bg_width = obs_properties_get(props, "fixed_bg_width");
	if (custom_width) {
		obs_property_set_visible(custom_width, word_wrap);
	}
	if (fixed_bg_width) {
		obs_property_set_visible(fixed_bg_width, word_wrap);
	}
	return true;
}

// Register source properties
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
	obs_properties_add_bool(group_layout, "bottom_align", "Centrar Verticalmente (Caja de altura fija)");

	obs_property_t *p_wrap =
		obs_properties_add_bool(group_layout, "word_wrap", "Activar Salto de Línea (Word Wrap)");
	obs_properties_add_int(group_layout, "custom_width", "Ancho Máximo (píxeles):", 100, 4096, 10);
	obs_properties_add_bool(group_layout, "fixed_bg_width", "Fondo de Ancho Fijo (Usa el ancho máximo)");

	obs_property_set_modified_callback(p_wrap, on_word_wrap_changed);

	obs_properties_add_group(props, "grp_layout", "3. Formato y Ajuste de Texto",
				 OBS_GROUP_NORMAL, group_layout);

	return props;
}

// Set default property values
static void my_font_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_int(settings, "outline_color", 0xFF000000);
	obs_data_set_default_bool(settings, "drop_shadow", false);
	obs_data_set_default_int(settings, "bg_color", 0x80000000);



	obs_data_set_default_bool(settings, "word_wrap", true);
	obs_data_set_default_int(settings, "custom_width", 900);
	obs_data_set_default_bool(settings, "fixed_bg_width", true);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_string(font_obj, "face", "Arial");
	obs_data_set_string(font_obj, "style", "Regular");
	obs_data_set_int(font_obj, "size", 45);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	obs_data_set_default_int(settings, "max_lines", 3);
	obs_data_set_default_bool(settings, "bottom_align", true);
}

// Update source configuration settings
static void my_font_update(void *data, obs_data_t *settings)
{
	MyCaptionsFont *ctx = (MyCaptionsFont *)data;
	
	long text_color = (long)obs_data_get_int(settings, "text_color");
	bool outline = obs_data_get_bool(settings, "outline");
	long outline_color = (long)obs_data_get_int(settings, "outline_color");
	bool drop_shadow = obs_data_get_bool(settings, "drop_shadow");

	bool word_wrap = obs_data_get_bool(settings, "word_wrap");
	long custom_width = (long)obs_data_get_int(settings, "custom_width");
	bool fixed_bg_width = obs_data_get_bool(settings, "fixed_bg_width");
	int max_lines = (int)obs_data_get_int(settings, "max_lines");
	bool bottom_align = obs_data_get_bool(settings, "bottom_align");
	// _is_partial is written by the audio filter, not shown in UI
	bool is_partial = obs_data_get_bool(settings, "_is_partial");

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (!font_obj) {
		font_obj = obs_data_get_default_obj(settings, "font");
	}

	int font_size = 45;
	std::string font_face = "Arial";
	std::string font_style = "Regular";

	if (font_obj) {
		font_size = (int)obs_data_get_int(font_obj, "size");
		if (font_size <= 0) font_size = 45;
		const char *face = obs_data_get_string(font_obj, "face");
		const char *style = obs_data_get_string(font_obj, "style");
		if (face) font_face = face;
		if (style) font_style = style;
		obs_data_release(font_obj);
	}

	const char *new_text = obs_data_get_string(settings, "text");
	std::string raw_text;
	if (new_text != nullptr && strlen(new_text) > 0) {
		raw_text = new_text;
		ctx->cached_raw_text = raw_text;
	} else {
		raw_text = ctx->cached_raw_text;
	}

	std::string text_to_set = raw_text;

	if (max_lines > 0 && word_wrap && custom_width > 0) {
		// Truncate leading words to fit max line count
		float avg_char_width = (float)font_size * 0.50f;
		int chars_per_line = (int)((float)custom_width / avg_char_width);
		if (chars_per_line < 1) chars_per_line = 20;

		// Split into words, then count chars line-by-line to see how many lines
		// the full text would occupy. Drop words from the front until it fits.
		std::vector<std::string> words;
		{
			std::string w;
			for (char c : text_to_set) {
				if (c == ' ' || c == '\n') {
					if (!w.empty()) { words.push_back(w); w.clear(); }
				} else {
					w += c;
				}
			}
			if (!w.empty()) words.push_back(w);
		}

		// Count how many display lines the word list would need
		auto count_lines = [&](size_t start) -> int {
			int lines = 1;
			int line_chars = 0;
			for (size_t i = start; i < words.size(); ++i) {
				int wlen = (int)utf8_char_count(words[i]);
				if (line_chars == 0) {
					line_chars = wlen;
				} else if (line_chars + 1 + wlen > chars_per_line) {
					lines++;
					line_chars = wlen;
				} else {
					line_chars += 1 + wlen;
				}
			}
			return lines;
		};

		// Drop words from the front until the text fits in max_lines
		size_t first = 0;
		while (first < words.size() && count_lines(first) > max_lines) {
			first++;
		}

		// Reassemble
		if (first > 0) {
			std::string trimmed;
			for (size_t i = first; i < words.size(); ++i) {
				if (i > first) trimmed += ' ';
				trimmed += words[i];
			}
			text_to_set = trimmed;
		}
	}

	bool text_changed = (ctx->cached_text != text_to_set);
	bool layout_changed = (ctx->cached_custom_width != custom_width ||
	                       ctx->cached_fixed_bg_width != (fixed_bg_width && word_wrap) ||
	                       ctx->cached_bottom_align != bottom_align ||
	                       ctx->cached_max_lines != max_lines ||
	                       ctx->cached_word_wrap != word_wrap);

	bool appearance_changed = (ctx->cached_text_color != text_color ||
	                           ctx->cached_outline != outline ||
	                           ctx->cached_outline_color != outline_color ||
	                           ctx->cached_drop_shadow != drop_shadow ||
	                           ctx->cached_font_face != font_face ||
	                           ctx->cached_font_size != font_size ||
	                           ctx->cached_font_style != font_style);
	
	// AFTER computing changes, update cached flags
	ctx->cached_is_partial = is_partial;

	if (text_changed || appearance_changed || layout_changed) {
		obs_data_t *text_settings = obs_data_create();
		obs_data_set_string(text_settings, "text", text_to_set.c_str());

		apply_text_color(text_settings, text_color);
		obs_data_set_bool(text_settings, "outline", outline);
		obs_data_set_int(text_settings, "outline_color", outline_color);
		obs_data_set_bool(text_settings, "drop_shadow", drop_shadow);

		obs_data_t *new_font_obj = obs_data_create();
		obs_data_set_string(new_font_obj, "face", font_face.c_str());
		obs_data_set_string(new_font_obj, "style", font_style.c_str());
		obs_data_set_int(new_font_obj, "size", font_size);
		obs_data_set_obj(text_settings, "font", new_font_obj);
		obs_data_release(new_font_obj);

		// Critical cross-platform wrap width properties:
		// Windows (text_gdiplus_v2) uses extents, extents_wrap, extents_cx, extents_cy
		// Linux/macOS (text_ft2_source) uses word_wrap, custom_width, cx
		obs_data_set_bool(text_settings, "extents", word_wrap);
		obs_data_set_bool(text_settings, "extents_wrap", word_wrap);
		obs_data_set_int(text_settings, "extents_cx", custom_width);
		obs_data_set_int(text_settings, "extents_cy", 0);

		obs_data_set_bool(text_settings, "word_wrap", word_wrap);
		obs_data_set_int(text_settings, "custom_width", custom_width);
		obs_data_set_int(text_settings, "cx", custom_width);

		// Update all cached state fields
		ctx->cached_text_color = text_color;
		ctx->cached_outline = outline;
		ctx->cached_outline_color = outline_color;
		ctx->cached_drop_shadow = drop_shadow;
		ctx->cached_font_face = font_face;
		ctx->cached_font_size = font_size;
		ctx->cached_font_style = font_style;
		ctx->cached_word_wrap = word_wrap;
		ctx->cached_custom_width = custom_width;
		ctx->cached_fixed_bg_width = fixed_bg_width && word_wrap;
		ctx->cached_bottom_align = bottom_align;
		ctx->cached_max_lines = max_lines;
		ctx->cached_text = text_to_set;

		obs_source_update(ctx->text_font, text_settings);
		obs_data_release(text_settings);
	}

	long bg_color = (long)obs_data_get_int(settings, "bg_color");
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

// Export OBS source info structure
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