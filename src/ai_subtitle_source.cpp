//
// Created by pce on 7/1/26.
//
#include "ai_subtitle_source.h"
#include "ai_subtitle_source.h"
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// --- Compatibilidad multiplataforma del motor de texto ---
//
// "text_ft2_source" (FreeType2) SOLO existe en Linux/Mac. En Windows, OBS
// usa un source completamente distinto para renderizar texto: "text_gdiplus_v2"
// (basado en GDI+, incluido de forma nativa en el propio sistema operativo).
// No comparten las mismas claves de configuración para el color, así que
// además de cambiar el ID también hay que adaptar cómo le mandamos el color
// del texto (ver apply_text_color más abajo).
#if defined(_WIN32)
#define TEXT_SOURCE_ID "text_gdiplus_v2"
#else
#define TEXT_SOURCE_ID "text_ft2_source"
#endif

// Aplica el color de texto usando la(s) clave(s) correcta(s) según el motor:
// - FreeType2 (Linux/Mac): usa "color1"/"color2" (degradado) + "color".
// - GDI+ (Windows): usa una sola clave "color".
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

// show data structure
struct MyCaptionsFont {
	obs_source_t *text_font;
	obs_source_t *color_font;
};

// --- Word wrap manual (nunca corta una palabra a la mitad) ---
//
// Tanto FreeType2 (word_wrap + custom_width) como GDI+ (extents_wrap +
// extents_cx/cy) tienen el mismo problema: si una palabra no cabe en el
// ancho configurado, la cortan letra por letra. Además, el modo "extents" de
// GDI+ obliga a usar una caja de tamaño FIJO (no crece según el contenido),
// lo cual rompe el efecto de "la caja de fondo se ajusta sola al texto" que
// ya tenías.
//
// Para evitar ambos problemas y que el comportamiento sea idéntico en
// Windows, Linux y Mac, hacemos el salto de línea NOSOTROS antes de mandarle
// el texto al source interno: insertamos "\n" solo entre palabras, nunca
// dentro de una. Así el source interno solo tiene que medir el texto ya
// wrappeado y crecer para ajustarse a él (sin activar su propio wrap).
//
// Como no tenemos acceso directo a las métricas reales del glyph desde aquí,
// aproximamos el ancho de cada palabra con un factor promedio por caracter
// relativo al tamaño de fuente. No es pixel-perfect, pero es más que
// suficiente para decidir dónde saltar de línea.
static float estimate_text_width(const std::string &text, int font_size)
{
	const float avg_char_width_ratio = 0.55f; // válido para fuentes tipo Arial Regular
	return (float)text.length() * (float)font_size * avg_char_width_ratio;
}

static std::string wrap_text_by_words(const std::string &input, int max_width_px, int font_size)
{
	if (max_width_px <= 0 || input.empty())
		return input;

	// Tokenizamos en palabras, conservando los saltos de línea manuales del usuario.
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

		// Saltamos de línea SOLO entre palabras, nunca dentro de una.
		if (!line.empty() && (line_width + space_width + word_width) > (float)max_width_px) {
			result += line;
			result += "\n";
			line.clear();
			line_width = 0.0f;
		}

		if (!line.empty()) {
			line += " ";
			line_width += space_width;
		}
		line += word;
		line_width += word_width;
	}
	result += line;

	return result;
}

static void *my_font_create(obs_data_t *settings, obs_source_t *source)
{
	(void)source;
	(void)settings; // Ya no lo pasamos directamente para evitar memory leaks/crashes

	MyCaptionsFont *data = (MyCaptionsFont *)malloc(sizeof(MyCaptionsFont));

	// 1. Configuramos el texto inicial ANTES del primer frame
	obs_data_t *text_defaults = obs_data_create();
	obs_data_set_string(text_defaults, "text", "Esperando IA...");
	apply_text_color(text_defaults, 0xFFFFFFFF);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_string(font_obj, "face", "Arial");
	obs_data_set_string(font_obj, "style", "Regular");
	obs_data_set_int(font_obj, "size", 150);
	obs_data_set_obj(text_defaults, "font", font_obj);
	obs_data_release(font_obj);

	// 2. Configuramos el fondo inicial (1x1 negro semitransparente) ANTES del primer frame
	obs_data_t *bg_defaults = obs_data_create();
	obs_data_set_int(bg_defaults, "width", 1);
	obs_data_set_int(bg_defaults, "height", 1);
	obs_data_set_int(bg_defaults, "color", 0x80000000);

	// 3. Creamos las fuentes privadas con sus configuraciones base (memoria independiente)
	//    TEXT_SOURCE_ID resuelve automáticamente a FreeType2 (Linux/Mac) o GDI+ (Windows).
	data->text_font = obs_source_create_private(TEXT_SOURCE_ID, "intern_text", text_defaults);
	data->color_font = obs_source_create_private("color_source", "intern_color", bg_defaults);

	// 4. Liberamos la memoria de las configuraciones base
	obs_data_release(text_defaults);
	obs_data_release(bg_defaults);

	return data;
}

// memory Clean
static void my_font_destroy(void *data)
{
	MyCaptionsFont *font_data = (MyCaptionsFont *)data;
	obs_source_release(font_data->text_font);
	obs_source_release(font_data->color_font);
	free(font_data);
}
// Draw the screen
static void my_font_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	MyCaptionsFont *ctx = (MyCaptionsFont *)data;

	uint32_t cx = obs_source_get_width(ctx->text_font);
	uint32_t cy = obs_source_get_height(ctx->text_font);

	// 1. Escalar la caja de color al tamaño exacto del texto + 20px
	gs_matrix_push();
	struct vec3 scale;
	vec3_set(&scale, (float)(cx + 20), (float)(cy + 20), 1.0f);
	gs_matrix_scale(&scale);
	obs_source_video_render(ctx->color_font);
	gs_matrix_pop();

	// 2. Movemos la matriz 10 píxeles para que el texto quede centrado en el margen de 20px
	gs_matrix_push();
	struct vec3 offset;
	vec3_set(&offset, 10.0f, 10.0f, 0.0f);
	gs_matrix_translate(&offset);

	// 3. Dibujamos el texto
	obs_source_video_render(ctx->text_font);

	// 4. Restauramos la matriz
	gs_matrix_pop();
}

// OBS REQUIERE SABER EL TAMAÑO (Ancho y Alto)
static uint32_t my_font_get_width(void *data)
{
	MyCaptionsFont *font_data = (MyCaptionsFont *)data;
	// El ancho real es el del texto MÁS los 20 píxeles de margen que le dimos a la caja
	return obs_source_get_width(font_data->text_font) + 20;
}

static uint32_t my_font_get_height(void *data)
{
	MyCaptionsFont *font_data = (MyCaptionsFont *)data;
	return obs_source_get_height(font_data->text_font) + 20;
}

// 1. Propiedades Visuales (El panel que verá el usuario en OBS)
static obs_properties_t *my_font_get_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_color_alpha(props, "text_color", "Color de Letras:");
	obs_properties_add_color_alpha(props, "bg_color", "Color de Fondo:");
	obs_properties_add_font(props, "font", "Tipografía:");

	// Controles de Word Wrap (Ajuste de Línea)
	obs_properties_add_bool(props, "word_wrap", "Activar Salto de Línea (Word Wrap)");
	obs_properties_add_int(props, "custom_width", "Ancho Máximo (píxeles):", 100, 4096, 10);

	// renglones maximos
	obs_properties_add_int(props, "max_lines", "Máximo de Renglones:", 1, 10, 1);

	return props;
}

// 2. Valores por Defecto
static void my_font_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF); // Blanco
	obs_data_set_default_int(settings, "bg_color", 0x80000000);   // Negro semi-transparente

	obs_data_set_default_bool(settings, "word_wrap", false);
	obs_data_set_default_int(settings, "custom_width", 800);

	// Tipografía por defecto (tamaño 150)
	obs_data_t *font_obj = obs_data_create();
	obs_data_set_string(font_obj, "face", "Arial"); // Nombre de la fuente
	obs_data_set_string(font_obj, "style", "Regular");
	obs_data_set_int(font_obj, "size", 150);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	// default renglones
	obs_data_set_default_int(settings, "max_lines", 3);
}

// 3. El Actualizador (Aquí hacemos la magia de la caja)
static void my_font_update(void *data, obs_data_t *settings)
{
	MyCaptionsFont *ctx = (MyCaptionsFont *)data;

	// --- A. Actualizar las letras ---
	obs_data_t *text_settings = obs_data_create();
	long text_color = obs_data_get_int(settings, "text_color");
	apply_text_color(text_settings, text_color);

	bool word_wrap = obs_data_get_bool(settings, "word_wrap");
	long custom_width = obs_data_get_int(settings, "custom_width");
	int max_lines = (int)obs_data_get_int(settings, "max_lines");

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");

	if (!font_obj) {
		font_obj = obs_data_get_default_obj(settings, "font");
	}

	int font_size = 150; // valor por defecto si todo lo demás falla

	if (font_obj) {
		font_size = (int)obs_data_get_int(font_obj, "size");
		if (font_size <= 0)
			font_size = 150;
		obs_data_set_obj(text_settings, "font", font_obj);
		obs_data_release(font_obj); // Muy importante liberar la referencia
	} else {
		// HARD FALLBACK: Si OBS falla catastróficamente en darnos el default, lo creamos manualmente
		obs_data_t *fallback_font = obs_data_create();
		obs_data_set_string(fallback_font, "face", "Arial");
		obs_data_set_string(fallback_font, "style", "Regular");
		obs_data_set_int(fallback_font, "size", 150);
		obs_data_set_obj(text_settings, "font", fallback_font);
		obs_data_release(fallback_font);
	}

	// Si el filtro de audio nos envió texto, se lo inyectamos
	const char *new_text = obs_data_get_string(settings, "text");
	std::string text_to_set = (new_text && strlen(new_text) > 0) ? new_text : "Esperando IA...";

	// Aplicamos nuestro word wrap manual (solo si el usuario lo activó en el panel)
	if (word_wrap) {
		text_to_set = wrap_text_by_words(text_to_set, (int)custom_width, font_size);
	}

	// Lógica de máximo de líneas (debe ir ANTES de pasárselo a OBS)
	if (max_lines > 0) {
		int newlines_found = 0;
		for (int i = text_to_set.length() - 1; i >= 0; --i) {
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

	// --- B. Actualizar el cuadro de fondo ---
	obs_data_t *bg_settings = obs_data_create();
	long bg_color = obs_data_get_int(settings, "bg_color");
	obs_data_set_int(bg_settings, "color", bg_color);

	// Ahora la caja se dibuja a 1x1 pixel, y la escalamos dinámicamente en 'my_font_render' frame por frame
	obs_data_set_int(bg_settings, "width", 1);
	obs_data_set_int(bg_settings, "height", 1);

	obs_source_update(ctx->color_font, bg_settings);
	obs_data_release(bg_settings);
}

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

	// Agregamos las funciones obligatorias de tamaño
	info.get_width = my_font_get_width;
	info.get_height = my_font_get_height;

	return info;
}