#include <assert.h>
#include <glib.h>

#include "rf-clipboard-text.h"

static void test_plain_text_is_unchanged(void)
{
	g_autofree char *text = rf_clipboard_text_normalize("plain text");

	assert(g_strcmp0(text, "plain text") == 0);
}

static void test_utf8_text_is_unchanged(void)
{
	g_autofree char *text = rf_clipboard_text_normalize("真实中文服务");

	assert(g_strcmp0(text, "真实中文服务") == 0);
}

static void test_unicode_escapes_are_decoded(void)
{
	g_autofree char *text = rf_clipboard_text_normalize(
		"Auth \\u6d41\\u7a0b\\u767b\\u5f55 xAI Grok \\u670d\\u52a1\\uff0c\\u81ea\\u52a8"
	);

	assert(g_strcmp0(text, "Auth 流程登录 xAI Grok 服务，自动") == 0);
}

static void test_mixed_json_escape_clipboard_text_is_decoded(void)
{
	g_autofree char *text = rf_clipboard_text_normalize(
		"PW******mf\n"
		"\thttps://ai.liaobots.work\t\\u65e0\t\n"
		"\\u6a21\\u578b46\\u8bf7\\u6c42\\u59340\n"
		"\t\n"
		"\\u5df2\\u505c\\u7528\n"
		"\\u6210\\u529f: 0\\u5931\\u8d25: 0\n"
		"--\n"
		"\t\n"
		"sk******1c\n"
		"\thttps://opusrelay.com"
	);

	assert(g_strcmp0(
		text,
		"PW******mf\n"
		"\thttps://ai.liaobots.work\t无\t\n"
		"模型46请求头0\n"
		"\t\n"
		"已停用\n"
		"成功: 0失败: 0\n"
		"--\n"
		"\t\n"
		"sk******1c\n"
		"\thttps://opusrelay.com"
	) == 0);
}

static void test_invalid_escapes_are_unchanged(void)
{
	g_autofree char *text = rf_clipboard_text_normalize("keep \\u12xz literal");

	assert(g_strcmp0(text, "keep \\u12xz literal") == 0);
}

static void test_ascii_only_escapes_are_unchanged(void)
{
	g_autofree char *text = rf_clipboard_text_normalize("\\u0041\\u0042");

	assert(g_strcmp0(text, "\\u0041\\u0042") == 0);
}

int main(void)
{
	test_plain_text_is_unchanged();
	test_utf8_text_is_unchanged();
	test_unicode_escapes_are_decoded();
	test_mixed_json_escape_clipboard_text_is_decoded();
	test_invalid_escapes_are_unchanged();
	test_ascii_only_escapes_are_unchanged();
	return 0;
}
