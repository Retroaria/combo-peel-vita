/*
 * ComboPeel - Instalador
 *
 * Copia o plugin para ur0:tai/ e adiciona a linha na secao *KERNEL do
 * config.txt (fazendo backup antes). Depois de instalar, este app pode
 * ser apagado: o plugin continua instalado.
 */

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <vita2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLUGIN_SRC      "app0:combo_peel.skprx"
#define PLUGIN_DST      "ur0:tai/combo_peel.skprx"
#define PLUGIN_LINE     "ur0:tai/combo_peel.skprx"
#define PLUGIN_NAME     "combo_peel.skprx"
#define CFG_UX0         "ux0:tai/config.txt"
#define CFG_UR0         "ur0:tai/config.txt"

#define MAX_FILE        (256 * 1024)
#define MAX_LINES       4096

static char g_msg[4][160];
static char g_backup[128];
static int  g_needs_reboot = 0;

static void set_msg(int i, const char *fmt, const char *arg)
{
	snprintf(g_msg[i], sizeof(g_msg[i]), fmt, arg ? arg : "");
}

static void clear_msgs(void)
{
	memset(g_msg, 0, sizeof(g_msg));
}

/* ------------------------------------------------------------------ */
/* Arquivos                                                           */
/* ------------------------------------------------------------------ */

static int file_exists(const char *path)
{
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

static int read_file(const char *path, char **out, int *len)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;

	char *buf = malloc(MAX_FILE + 1);
	if (!buf) {
		sceIoClose(fd);
		return -1;
	}

	int total = 0, r;
	while (total < MAX_FILE && (r = sceIoRead(fd, buf + total, MAX_FILE - total)) > 0)
		total += r;
	sceIoClose(fd);

	buf[total] = '\0';
	*out = buf;
	*len = total;
	return 0;
}

static int write_file(const char *path, const char *data, int len)
{
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return fd;

	int total = 0, r;
	while (total < len && (r = sceIoWrite(fd, data + total, len - total)) > 0)
		total += r;
	sceIoClose(fd);

	return total == len ? 0 : -1;
}

static int copy_file(const char *src, const char *dst)
{
	char *buf;
	int len, ret;

	if ((ret = read_file(src, &buf, &len)) < 0)
		return ret;
	ret = write_file(dst, buf, len);
	free(buf);
	return ret;
}

static const char *active_config(void)
{
	/* O taiHEN usa ux0:tai/config.txt se ele existir; senao ur0:tai/config.txt */
	return file_exists(CFG_UX0) ? CFG_UX0 : CFG_UR0;
}

/* ------------------------------------------------------------------ */
/* Edicao do config.txt                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	char *lines[MAX_LINES];
	int   count;
	int   crlf;
} Config;

static void cfg_free(Config *c)
{
	for (int i = 0; i < c->count; i++)
		free(c->lines[i]);
	c->count = 0;
}

static void cfg_parse(Config *c, const char *text)
{
	c->count = 0;
	c->crlf = strstr(text, "\r\n") != NULL;

	const char *p = text;
	while (*p && c->count < MAX_LINES) {
		const char *e = strchr(p, '\n');
		int n = e ? (int)(e - p) : (int)strlen(p);
		int m = n;
		if (m > 0 && p[m - 1] == '\r')
			m--;

		char *s = malloc(m + 1);
		memcpy(s, p, m);
		s[m] = '\0';
		c->lines[c->count++] = s;

		if (!e)
			break;
		p = e + 1;
	}
}

static char *cfg_join(Config *c, int *len)
{
	const char *eol = c->crlf ? "\r\n" : "\n";
	int total = 1;
	for (int i = 0; i < c->count; i++)
		total += strlen(c->lines[i]) + 2;

	char *out = malloc(total);
	out[0] = '\0';
	for (int i = 0; i < c->count; i++) {
		strcat(out, c->lines[i]);
		strcat(out, eol);
	}
	*len = strlen(out);
	return out;
}

static const char *trim_left(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static int is_plugin_line(const char *s)
{
	s = trim_left(s);
	return *s != '#' && strstr(s, PLUGIN_NAME) != NULL;
}

static int is_section(const char *s)
{
	return *trim_left(s) == '*';
}

static int is_kernel_section(const char *s)
{
	s = trim_left(s);
	return strncmp(s, "*KERNEL", 7) == 0;
}

static int is_blank_or_comment(const char *s)
{
	s = trim_left(s);
	return *s == '\0' || *s == '#';
}

static void cfg_insert(Config *c, int at, const char *text)
{
	if (c->count >= MAX_LINES)
		return;
	memmove(&c->lines[at + 1], &c->lines[at], (c->count - at) * sizeof(char *));
	c->lines[at] = strdup(text);
	c->count++;
}

static void cfg_remove(Config *c, int at)
{
	free(c->lines[at]);
	memmove(&c->lines[at], &c->lines[at + 1], (c->count - at - 1) * sizeof(char *));
	c->count--;
}

/* Retorna 1 se mudou algo */
static int cfg_add_plugin(Config *c)
{
	int changed = 0;

	/* Ja existe? Garante que aponta para o caminho certo */
	for (int i = 0; i < c->count; i++) {
		if (is_plugin_line(c->lines[i])) {
			if (strcmp(trim_left(c->lines[i]), PLUGIN_LINE) != 0) {
				free(c->lines[i]);
				c->lines[i] = strdup(PLUGIN_LINE);
				changed = 1;
			}
			return changed;
		}
	}

	/* Procura a secao *KERNEL */
	int k = -1;
	for (int i = 0; i < c->count; i++) {
		if (is_kernel_section(c->lines[i])) {
			k = i;
			break;
		}
	}

	if (k < 0) {
		cfg_insert(c, 0, "*KERNEL");
		cfg_insert(c, 1, PLUGIN_LINE);
		return 1;
	}

	/* Insere depois do ultimo plugin da secao *KERNEL */
	int at = k + 1;
	for (int i = k + 1; i < c->count && !is_section(c->lines[i]); i++) {
		if (!is_blank_or_comment(c->lines[i]))
			at = i + 1;
	}
	cfg_insert(c, at, PLUGIN_LINE);
	return 1;
}

static int cfg_remove_plugin(Config *c)
{
	int changed = 0;
	for (int i = c->count - 1; i >= 0; i--) {
		if (is_plugin_line(c->lines[i])) {
			cfg_remove(c, i);
			changed = 1;
		}
	}
	return changed;
}

static int config_has_plugin(const char *cfg_path)
{
	char *text;
	int len, found = 0;
	Config c;

	if (read_file(cfg_path, &text, &len) < 0)
		return 0;
	cfg_parse(&c, text);
	free(text);

	for (int i = 0; i < c.count; i++)
		if (is_plugin_line(c.lines[i]))
			found = 1;
	cfg_free(&c);
	return found;
}

/* Le, modifica e grava o config (com backup). Retorna <0 em erro, 0 sem mudanca, 1 mudou */
static int edit_config(const char *cfg_path, int install)
{
	char *text, *out;
	int len, out_len, changed;
	static Config c;

	if (read_file(cfg_path, &text, &len) < 0) {
		if (!install)
			return 0;
		text = strdup("");
		len = 0;
	}

	cfg_parse(&c, text);
	changed = install ? cfg_add_plugin(&c) : cfg_remove_plugin(&c);

	if (changed) {
		/* ex.: ur0:tai/config_2026-11-16_13-14_antes-de-instalar-combopeel.bak */
		SceDateTime t;
		char dir[64];
		const char *slash = strrchr(cfg_path, '/');
		int dlen = slash ? (int)(slash - cfg_path + 1) : 0;
		memcpy(dir, cfg_path, dlen);
		dir[dlen] = '\0';
		memset(&t, 0, sizeof(t));
		sceRtcGetCurrentClockLocalTime(&t);
		snprintf(g_backup, sizeof(g_backup),
			"%sconfig_%04d-%02d-%02d_%02d-%02d_antes-de-%s-combopeel.bak",
			dir, t.year, t.month, t.day, t.hour, t.minute,
			install ? "instalar" : "desinstalar");
		if (len > 0 && write_file(g_backup, text, len) < 0) {
			free(text);
			cfg_free(&c);
			return -2;
		}

		out = cfg_join(&c, &out_len);
		if (write_file(cfg_path, out, out_len) < 0) {
			free(out);
			free(text);
			cfg_free(&c);
			return -3;
		}
		free(out);
	}

	free(text);
	cfg_free(&c);
	return changed;
}

/* ------------------------------------------------------------------ */
/* Acoes                                                              */
/* ------------------------------------------------------------------ */

static void do_install(void)
{
	const char *cfg = active_config();
	clear_msgs();

	sceIoMkdir("ur0:tai", 0777);

	if (copy_file(PLUGIN_SRC, PLUGIN_DST) < 0) {
		set_msg(0, "ERRO: não consegui copiar o plugin para ur0:tai/.", NULL);
		set_msg(1, "Verifique se \"Enable Unsafe Homebrew\" está ativado.", NULL);
		return;
	}

	int r = edit_config(cfg, 1);
	if (r < 0) {
		set_msg(0, "ERRO ao editar %s. Nada foi alterado.", cfg);
		return;
	}

	set_msg(0, "Plugin instalado em " PLUGIN_DST "%s", "");
	if (r > 0)
		set_msg(1, "Linha adicionada em %s", cfg);
	else
		set_msg(1, "%s já estava configurado.", cfg);
	if (r > 0)
		set_msg(2, "Backup: %s", g_backup);
	set_msg(3, "Reinicie o Vita (SELECT). O plugin fica instalado mesmo se apagar a bolha.%s", "");
	g_needs_reboot = 1;
}

static void do_uninstall(void)
{
	const char *cfg = active_config();
	clear_msgs();

	int r = edit_config(cfg, 0);
	if (r < 0) {
		set_msg(0, "ERRO ao editar %s. Nada foi alterado.", cfg);
		return;
	}
	sceIoRemove(PLUGIN_DST);

	set_msg(0, "Plugin removido.%s", "");
	if (r > 0)
		set_msg(1, "Linha removida de %s", cfg);
	if (r > 0)
		set_msg(2, "Backup: %s", g_backup);
	set_msg(3, "Reinicie o Vita (SELECT) para concluir.%s", "");
	g_needs_reboot = 1;
}

/* ------------------------------------------------------------------ */
/* Interface                                                          */
/* ------------------------------------------------------------------ */

#define WHITE   RGBA8(0xFF, 0xFF, 0xFF, 0xFF)
#define GRAY    RGBA8(0xB0, 0xB8, 0xC8, 0xFF)
#define GREEN   RGBA8(0x70, 0xE0, 0x80, 0xFF)
#define YELLOW  RGBA8(0xFF, 0xD8, 0x60, 0xFF)

int main(void)
{
	SceCtrlData pad;
	unsigned int prev = 0;

	vita2d_init();
	vita2d_set_clear_color(RGBA8(0x14, 0x24, 0x40, 0xFF));
	vita2d_pgf *font = vita2d_load_default_pgf();

	const char *cfg = active_config();
	int installed = file_exists(PLUGIN_DST) && config_has_plugin(cfg);

	for (;;) {
		memset(&pad, 0, sizeof(pad));
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned int pressed = pad.buttons & ~prev;
		prev = pad.buttons;

		if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_TRIANGLE)) {
			if (pressed & SCE_CTRL_CROSS)
				do_install();
			else
				do_uninstall();
			cfg = active_config();
			installed = file_exists(PLUGIN_DST) && config_has_plugin(cfg);
		}
		else if ((pressed & SCE_CTRL_SELECT) && g_needs_reboot)
			scePowerRequestColdReset();
		else if (pressed & SCE_CTRL_START)
			break;

		vita2d_start_drawing();
		vita2d_clear_screen();

		vita2d_pgf_draw_text(font, 40, 60, WHITE, 1.6f, "ComboPeel - Instalador");
		vita2d_pgf_draw_text(font, 40, 100, GRAY, 1.0f,
			"Desbloqueia a tela de bloqueio com: analógico esquerdo para baixo + X");

		vita2d_pgf_draw_text(font, 40, 150, GRAY, 1.0f, "Configuração ativa:");
		vita2d_pgf_draw_text(font, 260, 150, WHITE, 1.0f, cfg);
		vita2d_pgf_draw_text(font, 40, 180, GRAY, 1.0f, "Situação:");
		vita2d_pgf_draw_text(font, 260, 180, installed ? GREEN : YELLOW, 1.0f,
			installed ? "instalado" : "não instalado");

		vita2d_pgf_draw_text(font, 40, 250, WHITE, 1.1f, "X          Instalar / atualizar");
		vita2d_pgf_draw_text(font, 40, 285, WHITE, 1.1f, "Triângulo  Desinstalar (remove o plugin)");
		if (g_needs_reboot)
			vita2d_pgf_draw_text(font, 40, 320, YELLOW, 1.1f, "SELECT     Reiniciar o Vita agora");
		vita2d_pgf_draw_text(font, 40, 355, WHITE, 1.1f, "START      Sair");

		for (int i = 0; i < 4; i++)
			if (g_msg[i][0])
				vita2d_pgf_draw_text(font, 40, 410 + i * 30, i == 0 ? GREEN : WHITE, 0.9f, g_msg[i]);

		vita2d_end_drawing();
		vita2d_swap_buffers();
	}

	vita2d_fini();
	vita2d_free_pgf(font);
	sceKernelExitProcess(0);
	return 0;
}
