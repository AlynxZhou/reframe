#include "rf-clipboard-text.h"

#include <stdbool.h>
#include <string.h>

#include <glib.h>

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool read_u_escape(const char *text, gunichar *value)
{
	gunichar result = 0;

	if (text == NULL || text[0] != '\\' || text[1] != 'u' || value == NULL)
		return false;

	for (unsigned int i = 0; i < 4; ++i) {
		const int digit = hex_value(text[2 + i]);

		if (digit < 0)
			return false;
		result = (result << 4) | (gunichar)digit;
	}
	*value = result;
	return true;
}

static bool append_u_escape(
	GString *out,
	const char **cursor,
	bool *decoded_non_ascii
)
{
	gunichar first = 0;
	gunichar value = 0;
	const char *text = *cursor;

	if (!read_u_escape(text, &first))
		return false;

	if (first >= 0xd800 && first <= 0xdbff &&
	    read_u_escape(text + 6, &value) &&
	    value >= 0xdc00 && value <= 0xdfff) {
		value = 0x10000 +
			((first - 0xd800) << 10) +
			(value - 0xdc00);
		*cursor += 12;
	} else {
		value = first;
		*cursor += 6;
	}

	if (!g_unichar_validate(value))
		return false;
	if (value > 0x7f)
		*decoded_non_ascii = true;
	g_string_append_unichar(out, value);
	return true;
}

char *rf_clipboard_text_normalize(const char *text)
{
	bool decoded_non_ascii = false;
	const char *cursor = text;
	g_autoptr(GString) out = NULL;

	if (text == NULL)
		return NULL;
	if (strstr(text, "\\u") == NULL)
		return g_strdup(text);

	out = g_string_sized_new(strlen(text));
	while (*cursor != '\0') {
		if (cursor[0] == '\\' && cursor[1] == 'u' &&
		    append_u_escape(out, &cursor, &decoded_non_ascii))
			continue;

		g_string_append_c(out, *cursor);
		cursor++;
	}

	if (!decoded_non_ascii)
		return g_strdup(text);
	return g_strdup(out->str);
}
