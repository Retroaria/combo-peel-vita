/*
 * ComboPeel - PS Vita kernel plugin
 *
 * Na tela de bloqueio, ao apertar  ANALOGICO ESQUERDO PARA BAIXO + X,
 * o plugin simula o gesto de "descascar" (peel) a tela, arrastando um
 * dedo virtual do canto superior direito ate o centro da tela.
 *
 * Seguranca contra acionamento acidental:
 *  - o combo so funciona por alguns segundos depois que o Vita acorda
 *    do modo de espera (ou logo apos ligar);
 *  - funciona uma unica vez por despertar;
 *  - qualquer toque real na tela desarma o combo.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/power.h>
#include <taihen.h>

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

/* ------------------------------------------------------------------ */
/* Estado                                                             */
/* ------------------------------------------------------------------ */

static volatile int      g_run = 1;
static volatile int      g_armed = 0;
static volatile SceInt64 g_armed_until = 0;
static volatile int      g_injecting = 0;
static volatile SceInt64 g_inject_start = 0;

static SceUID g_thread = -1;

/* ------------------------------------------------------------------ */
/* Hooks do touch (NIDs das funcoes de kernel do SceTouch)            */
/* ------------------------------------------------------------------ */

#define NID_ksceTouchPeek        0xBAD1960B
#define NID_ksceTouchPeekRegion  0x9B3F7207
#define NID_ksceTouchRead        0x70C8AACE
#define NID_ksceTouchReadRegion  0x9A91F624

static tai_hook_ref_t ref_peek, ref_peek_region, ref_read, ref_read_region;
static SceUID uid_peek = -1, uid_peek_region = -1, uid_read = -1, uid_read_region = -1;

static void arm(SceInt64 window_us)
{
	g_armed_until = ksceKernelGetSystemTimeWide() + window_us;
	g_armed = 1;
}

/* Calcula a posicao do dedo virtual. Retorna 1 se o dedo esta na tela. */
static int fake_finger(SceInt64 now, int *x, int *y)
{
	int t = (int)(now - g_inject_start);

	if (t < 0)
		t = 0;

	if (t < SWIPE_MOVE_US) {
		/* ease-out: comeca rapido e desacelera, como um dedo de verdade */
		int p  = t / 1000;                    /* 0..400 ms              */
		int d  = SWIPE_MOVE_US / 1000;        /* 400                    */
		int q  = d - p;                       /* tempo restante         */
		int k  = d * d - q * q;               /* 0..d^2 (ease-out quad) */
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
	SceInt64 now;

	if (port != TOUCH_PORT_FRONT || count <= 0 || data == NULL)
		return;

	if (!g_injecting) {
		/* Toque real na tela desarma o combo */
		if (g_armed) {
			for (i = 0; i < count; i++) {
				if (data[i].reportNum > 0) {
					g_armed = 0;
					break;
				}
			}
		}
		return;
	}

	now  = ksceKernelGetSystemTimeWide();
	down = fake_finger(now, &x, &y);

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
	if (ret > 0)
		patch_touch(port, data, ret);
	return ret;
}

static int hook_peek_region(SceUInt32 port, TouchData *data, SceUInt32 n, int region)
{
	int ret = TAI_CONTINUE(int, ref_peek_region, port, data, n, region);
	if (ret > 0)
		patch_touch(port, data, ret);
	return ret;
}

static int hook_read(SceUInt32 port, TouchData *data, SceUInt32 n)
{
	int ret = TAI_CONTINUE(int, ref_read, port, data, n);
	if (ret > 0)
		patch_touch(port, data, ret);
	return ret;
}

static int hook_read_region(SceUInt32 port, TouchData *data, SceUInt32 n, int region)
{
	int ret = TAI_CONTINUE(int, ref_read_region, port, data, n, region);
	if (ret > 0)
		patch_touch(port, data, ret);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Deteccao de "acordou do modo de espera"                            */
/* ------------------------------------------------------------------ */

#define RESUME_FLAGS (SCE_POWER_CB_SYSTEM_RESUMING | SCE_POWER_CB_SYSTEM_RESUME | \
                      SCE_POWER_CB_AFTER_SYSTEM_RESUME)

static int power_callback(int notify_id, int notify_count, int power_info, void *common)
{
	(void)notify_id; (void)notify_count; (void)common;

	if (power_info & RESUME_FLAGS) {
		g_injecting = 0;
		arm(ARM_WINDOW_US);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Thread principal: le os botoes e dispara o gesto                   */
/* ------------------------------------------------------------------ */

static int combo_thread(SceSize args, void *argp)
{
	SceCtrlData pad;
	SceUID cb;
	int prev_combo = 1; /* exige soltar e apertar de novo */
	SceInt64 last_loop = ksceKernelGetSystemTimeWide();

	(void)args; (void)argp;

	cb = ksceKernelCreateCallback("ComboPeelPower", 0, power_callback, NULL);
	if (cb >= 0)
		kscePowerRegisterCallback(cb);

	ksceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

	while (g_run) {
		SceInt64 now = ksceKernelGetSystemTimeWide();
		int combo = 0;

		/* Plano B: se o loop ficou parado muito tempo, o Vita dormiu */
		if (now - last_loop > 2 * 1000 * 1000) {
			g_injecting = 0;
			arm(ARM_WINDOW_US);
		}
		last_loop = now;

		/* Encerra o gesto */
		if (g_injecting &&
		    now - g_inject_start > SWIPE_MOVE_US + SWIPE_HOLD_US + SWIPE_RELEASE_US)
			g_injecting = 0;

		/* Janela expirou */
		if (g_armed && now > g_armed_until)
			g_armed = 0;

		if (ksceCtrlPeekBufferPositive(0, &pad, 1) > 0)
			combo = (pad.buttons & COMBO_BUTTON) && pad.ly >= STICK_DOWN_MIN;

		if (combo && !prev_combo && g_armed && !g_injecting) {
			g_armed = 0;
			g_inject_start = now;
			g_injecting = 1;
		}
		prev_combo = combo;

		ksceKernelDelayThreadCB(POLL_INTERVAL_US);
	}

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

	uid_peek = taiHookFunctionExportForKernel(KERNEL_PID, &ref_peek,
		"SceTouch", TAI_ANY_LIBRARY, NID_ksceTouchPeek, hook_peek);
	uid_peek_region = taiHookFunctionExportForKernel(KERNEL_PID, &ref_peek_region,
		"SceTouch", TAI_ANY_LIBRARY, NID_ksceTouchPeekRegion, hook_peek_region);
	uid_read = taiHookFunctionExportForKernel(KERNEL_PID, &ref_read,
		"SceTouch", TAI_ANY_LIBRARY, NID_ksceTouchRead, hook_read);
	uid_read_region = taiHookFunctionExportForKernel(KERNEL_PID, &ref_read_region,
		"SceTouch", TAI_ANY_LIBRARY, NID_ksceTouchReadRegion, hook_read_region);

	/* Tela de bloqueio logo depois de ligar o Vita */
	arm(BOOT_WINDOW_US);

	g_thread = ksceKernelCreateThread("ComboPeel", combo_thread, 0x3C, 0x2000, 0, 0x10000, 0);
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
