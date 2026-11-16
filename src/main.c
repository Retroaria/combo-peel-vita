/*
 * ComboPeel - PS Vita kernel plugin  (v1.1 - com registro de diagnostico)
 *
 * Na tela de bloqueio, ao apertar  ANALOGICO ESQUERDO PARA BAIXO + X,
 * o plugin simula o gesto de "descascar" (peel) a tela, arrastando um
 * dedo virtual do canto superior direito ate o centro da tela.
 *
 * Seguranca contra acionamento acidental:
 *  - o combo so funciona nos segundos seguintes a: Vita acordar, botao
 *    Power ser apertado, ou o Vita ser ligado;
 *  - funciona uma unica vez por janela;
 *  - NAO arma com o botao PS (dentro de um jogo o PS leva ao LiveArea,
 *    onde o mesmo gesto fecharia o jogo).
 *
 * Registro: ux0:data/combopeel.log (ou ur0:data/combopeel.log se o
 * cartao ainda nao estiver montado).
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/power.h>
#include <taihen.h>
#include <stdarg.h>

#define PLUGIN_VERSION "1.1-diag"

/* Estruturas do touch (iguais as do SDK de usuario, psp2/touch.h) */
typedef struct {
	SceUInt8  id;
	SceUInt8  force;
	SceUInt16 x;
	SceUInt16 y;
	SceUInt8  reserved[8];
	SceUInt16 info;
} TouchReport;          /* 0x10 bytes */

typedef struct {
	SceUInt64   timeStamp;
	SceUInt32   status;
	SceUInt32   reportNum;
	TouchReport report[8];
} TouchData;            /* 0x90 bytes */

_Static_assert(sizeof(TouchReport) == 0x10, "TouchReport size");
_Static_assert(sizeof(TouchData) == 0x90, "TouchData size");

#define TOUCH_PORT_FRONT 0

/* ------------------------------------------------------------------ */
/* Configuracao                                                       */
/* ------------------------------------------------------------------ */

#define COMBO_BUTTON        SCE_CTRL_CROSS  /* botao X                         */
#define STICK_DOWN_MIN      200             /* ly: 0 = cima, 128 = centro, 255 = baixo */

#define ARM_WINDOW_US       (20 * 1000 * 1000)  /* janela apos acordar: 20 s   */
#define BOOT_WINDOW_US      (120 * 1000 * 1000) /* janela apos ligar: 120 s    */
#define GAP_RESUME_US       (2 * 1000 * 1000)   /* loop parado > 2 s = dormiu  */

/* Gesto (coordenadas do painel frontal: 1920 x 1088) */
#define SWIPE_X0            1890
#define SWIPE_Y0            30
#define SWIPE_X1            700
#define SWIPE_Y1            760
#define SWIPE_MOVE_US       400000  /* tempo arrastando                        */
#define SWIPE_HOLD_US       60000   /* segura no ponto final                   */
#define SWIPE_RELEASE_US    150000  /* "dedo levantado" antes de encerrar      */
#define FAKE_TOUCH_ID       0x50
#define FAKE_TOUCH_FORCE    128

#define POLL_INTERVAL_US    10000
#define ANALOG_REFRESH_US   (1000 * 1000)

/* Botoes de sistema (bits do SceCtrlData.buttons no kernel) */
#define BTN_PS              0x00010000
#define BTN_POWER           0x40000000

/* Flags do callback de energia */
#define PCB_RESUME_FLAGS    (0x00000080 /* AFTER_SYSTEM_RESUME */ | \
                             0x00020000 /* SYSTEM_RESUMING     */ | \
                             0x00040000 /* SYSTEM_RESUME       */)
#define PCB_POWER_BUTTON    (0x40000000 /* BUTTON_POWER_HOLD   */ | \
                             0x80000000 /* BUTTON_POWER_PRESS  */)

/* ------------------------------------------------------------------ */
/* Estado                                                             */
/* ------------------------------------------------------------------ */

static volatile int      g_run = 1;
static volatile int      g_armed = 0;
static volatile SceInt64 g_armed_until = 0;
static volatile int      g_injecting = 0;
static volatile SceInt64 g_inject_start = 0;

/* contadores de chamadas do touch durante o gesto (diagnostico) */
static volatile int g_cnt_peek, g_cnt_peek_region, g_cnt_read, g_cnt_read_region;

static SceUID g_thread = -1;

/* ------------------------------------------------------------------ */
/* Registro (log)                                                     */
/* So a thread do plugin escreve no log (o callback de energia roda    */
/* dentro dela), entao nao precisa de trava.                          */
/* ------------------------------------------------------------------ */

#define LOG_BUF_SIZE 8192

static char g_log[LOG_BUF_SIZE];
static int  g_log_len = 0;
static int  g_log_first_flush = 1;

static void log_event(const char *fmt, ...)
{
	char line[192];
	va_list ap;
	int n, t;
	SceInt64 now = ksceKernelGetSystemTimeWide();

	t = (int)(now / 1000); /* ms desde que ligou */
	n = snprintf(line, sizeof(line), "[%6d.%03d] ", t / 1000, t % 1000);

	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
	va_end(ap);

	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\r';
	line[n++] = '\n';

	if (g_log_len + n <= LOG_BUF_SIZE) {
		memcpy(g_log + g_log_len, line, n);
		g_log_len += n;
	}
}

static int log_try_write(const char *dir, const char *path)
{
	SceUID fd;
	SceIoStat st;
	int flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND;

	/* No primeiro registro de cada boot, zera o arquivo se ficou grande */
	if (g_log_first_flush && ksceIoGetstat(path, &st) >= 0 && st.st_size > 256 * 1024)
		flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;

	ksceIoMkdir(dir, 0777);
	fd = ksceIoOpen(path, flags, 0666);
	if (fd < 0)
		return fd;

	ksceIoWrite(fd, g_log, g_log_len);
	ksceIoClose(fd);
	return 0;
}

static void log_flush(void)
{
	if (g_log_len == 0)
		return;

	if (log_try_write("ux0:data", "ux0:data/combopeel.log") < 0 &&
	    log_try_write("ur0:data", "ur0:data/combopeel.log") < 0)
		return; /* tenta de novo depois */

	g_log_len = 0;
	g_log_first_flush = 0;
}

/* ------------------------------------------------------------------ */
/* Armar / desarmar                                                   */
/* ------------------------------------------------------------------ */

static void ensure_analog(const char *why)
{
	static int last_err = 0;
	int prev = ksceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

	if (prev < 0) {
		if (prev != last_err)
			log_event("analogico: erro %08X ao ligar modo (%s)", prev, why);
		last_err = prev;
		return;
	}
	last_err = 0;
	if (prev != SCE_CTRL_MODE_ANALOG)
		log_event("analogico: modo estava %d, religado (%s)", prev, why);
}

static void arm(SceInt64 window_us, const char *why)
{
	g_injecting = 0;
	g_armed_until = ksceKernelGetSystemTimeWide() + window_us;
	g_armed = 1;
	log_event("ARMADO por %d s (%s)", (int)(window_us / 1000000), why);
	ensure_analog(why);
}

/* ------------------------------------------------------------------ */
/* Hooks do touch (NIDs das funcoes de kernel do SceTouch)            */
/* ------------------------------------------------------------------ */

#define NID_ksceTouchPeek        0xBAD1960B
#define NID_ksceTouchPeekRegion  0x9B3F7207
#define NID_ksceTouchRead        0x70C8AACE
#define NID_ksceTouchReadRegion  0x9A91F624

static tai_hook_ref_t ref_peek, ref_peek_region, ref_read, ref_read_region;
static SceUID uid_peek = -1, uid_peek_region = -1, uid_read = -1, uid_read_region = -1;

/* Calcula a posicao do dedo virtual. Retorna 1 se o dedo esta na tela. */
static int fake_finger(SceInt64 now, int *x, int *y)
{
	int t = (int)(now - g_inject_start);

	if (t < 0)
		t = 0;

	if (t < SWIPE_MOVE_US) {
		/* ease-out: comeca rapido e desacelera, como um dedo de verdade */
		int p  = t / 1000;
		int d  = SWIPE_MOVE_US / 1000;
		int q  = d - p;
		int k  = d * d - q * q;
		*x = SWIPE_X0 + ((SWIPE_X1 - SWIPE_X0) * k) / (d * d);
		*y = SWIPE_Y0 + ((SWIPE_Y1 - SWIPE_Y0) * k) / (d * d);
		return 1;
	}

	if (t < SWIPE_MOVE_US + SWIPE_HOLD_US) {
		*x = SWIPE_X1;
		*y = SWIPE_Y1;
		return 1;
	}

	return 0;
}

static void patch_touch(SceUInt32 port, TouchData *data, int count)
{
	int i, x, y, down;

	if (!g_injecting || port != TOUCH_PORT_FRONT || count <= 0 || data == NULL)
		return;

	down = fake_finger(ksceKernelGetSystemTimeWide(), &x, &y);

	for (i = 0; i < count; i++) {
		TouchData *d = &data[i];

		if (down) {
			d->reportNum            = 1;
			d->report[0].id         = FAKE_TOUCH_ID;
			d->report[0].force      = FAKE_TOUCH_FORCE;
			d->report[0].x          = (SceUInt16)x;
			d->report[0].y          = (SceUInt16)y;
			d->report[0].info       = 0;
		} else {
			d->reportNum = 0;
		}
	}
}

static int hook_peek(SceUInt32 port, TouchData *data, SceUInt32 n)
{
	int ret = TAI_CONTINUE(int, ref_peek, port, data, n);
	if (ret > 0 && g_injecting) {
		g_cnt_peek++;
		patch_touch(port, data, ret);
	}
	return ret;
}

static int hook_peek_region(SceUInt32 port, TouchData *data, SceUInt32 n, int region)
{
	int ret = TAI_CONTINUE(int, ref_peek_region, port, data, n, region);
	if (ret > 0 && g_injecting) {
		g_cnt_peek_region++;
		patch_touch(port, data, ret);
	}
	return ret;
}

static int hook_read(SceUInt32 port, TouchData *data, SceUInt32 n)
{
	int ret = TAI_CONTINUE(int, ref_read, port, data, n);
	if (ret > 0 && g_injecting) {
		g_cnt_read++;
		patch_touch(port, data, ret);
	}
	return ret;
}

static int hook_read_region(SceUInt32 port, TouchData *data, SceUInt32 n, int region)
{
	int ret = TAI_CONTINUE(int, ref_read_region, port, data, n, region);
	if (ret > 0 && g_injecting) {
		g_cnt_read_region++;
		patch_touch(port, data, ret);
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* Callback de energia (roda dentro da thread do plugin)              */
/* ------------------------------------------------------------------ */

static int power_callback(int notify_id, int notify_count, int power_info, void *common)
{
	(void)notify_id; (void)notify_count; (void)common;

	log_event("energia: flags=%08X", (unsigned)power_info);

	if (power_info & PCB_RESUME_FLAGS)
		arm(ARM_WINDOW_US, "acordou");
	else if (power_info & PCB_POWER_BUTTON)
		arm(ARM_WINDOW_US, "botao Power (callback)");

	return 0;
}

/* ------------------------------------------------------------------ */
/* Thread principal                                                   */
/* ------------------------------------------------------------------ */

static int combo_thread(SceSize args, void *argp)
{
	SceCtrlData pad;
	SceUID cb;
	unsigned int prev_buttons = 0;
	int prev_combo = 1; /* exige soltar e apertar de novo */
	SceInt64 last_loop = ksceKernelGetSystemTimeWide();
	SceInt64 last_analog = 0, last_flush = 0;

	(void)args; (void)argp;

	cb = ksceKernelCreateCallback("ComboPeelPower", 0, power_callback, NULL);
	log_event("callback de energia: cb=%08X reg=%08X", cb,
	     cb >= 0 ? kscePowerRegisterCallback(cb) : -1);

	ensure_analog("inicio");

	while (g_run) {
		SceInt64 now = ksceKernelGetSystemTimeWide();
		int combo = 0, ret;
		unsigned int buttons = 0, pressed;

		/* Plano B: se o loop ficou parado, o Vita dormiu */
		if (now - last_loop > GAP_RESUME_US) {
			log_event("loop parado por %d ms", (int)((now - last_loop) / 1000));
			arm(ARM_WINDOW_US, "pausa longa");
		}
		last_loop = now;

		if (now - last_analog > ANALOG_REFRESH_US) {
			ensure_analog("verificacao periodica");
			last_analog = now;
		}

		/* Encerra o gesto */
		if (g_injecting &&
		    now - g_inject_start > SWIPE_MOVE_US + SWIPE_HOLD_US + SWIPE_RELEASE_US) {
			g_injecting = 0;
			log_event("gesto terminou; touch lido: peek=%d peekR=%d read=%d readR=%d",
			     g_cnt_peek, g_cnt_peek_region, g_cnt_read, g_cnt_read_region);
		}

		/* Janela expirou */
		if (g_armed && now > g_armed_until) {
			g_armed = 0;
			log_event("desarmado (tempo esgotado)");
		}

		memset(&pad, 0, sizeof(pad));
		ret = ksceCtrlPeekBufferPositive(0, &pad, 1);
		if (ret > 0) {
			buttons = pad.buttons;
			combo = (buttons & COMBO_BUTTON) && pad.ly >= STICK_DOWN_MIN;
		}
		pressed = buttons & ~prev_buttons;

		if (pressed & BTN_POWER)
			arm(ARM_WINDOW_US, "botao Power");
		if (pressed & BTN_PS)
			log_event("botao PS apertado (nao arma)");

		/* Registra cada aperto de X, com o estado do analogico */
		if (pressed & COMBO_BUTTON) {
			int left = g_armed ? (int)((g_armed_until - now) / 1000) : 0;
			log_event("X apertado: ly=%d lx=%d botoes=%08X armado=%d resta=%d ms ret=%d",
			     pad.ly, pad.lx, buttons, g_armed, left, ret);
		}

		if (combo && !prev_combo) {
			if (g_armed && !g_injecting) {
				g_armed = 0;
				g_cnt_peek = g_cnt_peek_region = g_cnt_read = g_cnt_read_region = 0;
				g_inject_start = now;
				g_injecting = 1;
				log_event("COMBO -> gesto disparado");
			} else {
				log_event("combo ignorado (armado=%d, gesto em andamento=%d)",
				     g_armed, g_injecting);
			}
		}
		prev_combo = combo;
		prev_buttons = buttons;

		if (now - last_flush > 1000 * 1000) {
			log_flush();
			last_flush = now;
		}

		ksceKernelDelayThreadCB(POLL_INTERVAL_US);
	}

	log_event("thread encerrada");
	log_flush();

	if (cb >= 0) {
		kscePowerUnregisterCallback(cb);
		ksceKernelDeleteCallback(cb);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Entrada / saida do modulo                                          */
/* ------------------------------------------------------------------ */

void _start() __attribute__ ((weak, alias("module_start")));

int module_start(SceSize args, void *argp)
{
	(void)args; (void)argp;

	log_event("==== ComboPeel " PLUGIN_VERSION " iniciado ====");

	uid_peek = taiHookFunctionExportForKernel(KERNEL_PID, &ref_peek,
		"SceTouch", TAI_ANY_LIBRARY, NID_ksceTouchPeek, hook_peek);
	uid_peek_region = taiHookFunctionExportForKernel(KERNEL_PID, &ref_peek_region,
		"SceTouch", TAI_ANY_LIBRARY, NID_ksceTouchPeekRegion, hook_peek_region);
	uid_read = taiHookFunctionExportForKernel(KERNEL_PID, &ref_read,
		"SceTouch", TAI_ANY_LIBRARY, NID_ksceTouchRead, hook_read);
	uid_read_region = taiHookFunctionExportForKernel(KERNEL_PID, &ref_read_region,
		"SceTouch", TAI_ANY_LIBRARY, NID_ksceTouchReadRegion, hook_read_region);

	log_event("hooks touch: peek=%08X peekR=%08X read=%08X readR=%08X",
	     uid_peek, uid_peek_region, uid_read, uid_read_region);

	/* Tela de bloqueio logo depois de ligar o Vita */
	arm(BOOT_WINDOW_US, "Vita ligado");

	g_thread = ksceKernelCreateThread("ComboPeel", combo_thread, 0x3C, 0x3000, 0, 0x10000, 0);
	if (g_thread >= 0)
		ksceKernelStartThread(g_thread, 0, NULL);

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;

	g_run = 0;
	g_injecting = 0;

	if (g_thread >= 0) {
		ksceKernelWaitThreadEnd(g_thread, NULL, NULL);
		ksceKernelDeleteThread(g_thread);
	}

	if (uid_peek >= 0)        taiHookReleaseForKernel(uid_peek, ref_peek);
	if (uid_peek_region >= 0) taiHookReleaseForKernel(uid_peek_region, ref_peek_region);
	if (uid_read >= 0)        taiHookReleaseForKernel(uid_read, ref_read);
	if (uid_read_region >= 0) taiHookReleaseForKernel(uid_read_region, ref_read_region);

	return SCE_KERNEL_STOP_SUCCESS;
}
