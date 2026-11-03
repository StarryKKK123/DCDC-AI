/* app_openloop.c - F28P55x 300W板开环发波 (基于C2000Ware driverlib)
 * 结构: EPWM1=桥A(master) EPWM2=桥B(移相), 互补+死区, up-down计数
 * 时钟: SYSCLK=150MHz -> TBCLK=150MHz, fs=150MHz/(2*TBPRD)
 * 占空比约定: D = EPWMxA高电平占比 (cmpa=(1-D)*TBPRD); 驱动反相时用命令'X'翻转
 *
 * 上电流程: 串口banner -> GPIO0翻转3次(诊断) -> 软启动(10级) -> 发波+每秒状态
 * 串口(XDS110背板, 115200): F<kHz> A<D1%> B<D2%> P<phi_ns> T<dt_ns> X(翻转极性) S(状态) R(重软启)
 */

#include "device.h"
#include "driverlib.h"
#include <stdint.h>
#include <string.h>

/* ===================== 工况参数(上电默认, 可串口在线改) ===================== */
#define OL_FS_KHZ   200.0f     /* 开关频率 kHz (300W LUT: 48->24V/4A点) */
#define OL_D1       0.3603f    /* 桥A占空比(EPWM1A高电平占比) */
#define OL_D2       0.6961f    /* 桥B占空比 */
#define OL_PHI_NS   747.0f     /* 桥B相对桥A移相 ns */
#define OL_DT_NS    100.0f     /* 死区 ns */
/* =========================================================================== */

#define PWM_CLK     150000000.0f   /* SYSCLK=150MHz (device.h: 20M*30/4) */
#define SCIA_BAUD   115200.0f

static volatile float g_fs = OL_FS_KHZ;
static volatile float g_d1 = OL_D1;
static volatile float g_d2 = OL_D2;
static volatile float g_phi = OL_PHI_NS;
static volatile float g_dt  = OL_DT_NS;
static volatile uint32_t g_invert = 0U;    /* 1=D用1-D(驱动反相时) */
static volatile uint32_t g_hb = 0U;
static volatile uint32_t g_seq = 0U;       /* 参数版本号(改一次+1) */

/* ---------------- 串口 ---------------- */
static void scia_init(void)
{
    GPIO_setPinConfig(GPIO_28_SCIA_RX);
    GPIO_setPinConfig(GPIO_29_SCIA_TX);
    GPIO_setPadConfig(28U, GPIO_PIN_TYPE_STD);
    GPIO_setPadConfig(29U, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(28U, GPIO_QUAL_SYNC);

    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_SCIA);
    SCI_disableFIFO(SCIA_BASE);
    SCI_performSoftwareReset(SCIA_BASE);
    SCI_setConfig(SCIA_BASE, DEVICE_LSPCLK_FREQ, 115200,
                  (SCI_CONFIG_WLEN_8 | SCI_CONFIG_STOP_ONE | SCI_CONFIG_PAR_NONE));
    SCI_resetChannels(SCIA_BASE);
    SCI_resetRxFIFO(SCIA_BASE);
    SCI_resetTxFIFO(SCIA_BASE);
    SCI_clearInterruptStatus(SCIA_BASE, SCI_INT_TXFF | SCI_INT_RXFF);
    SCI_enableModule(SCIA_BASE);
    SCI_performSoftwareReset(SCIA_BASE);
    SCI_enableFIFO(SCIA_BASE);
    SCI_enableInterrupt(SCIA_BASE, SCI_INT_RXFF);
    SCI_resetTxFIFO(SCIA_BASE);
}

static void putc_(char c)
{
    volatile uint32_t guard = 0U;
    while (SCI_getTxFIFOStatus(SCIA_BASE) >= SCI_FIFO_TX15 && guard < 200000U) guard++;
    if (guard < 200000U)
        SCI_writeCharNonBlocking(SCIA_BASE, (uint16_t)(uint8_t)c);
    /* 超时丢字: 串口故障不阻塞发波 */
}

/* printf过重, 自带轻量puts */
static void puts_(const char *s)
{
    while (*s) putc_(*s++);
}

static void putu_(uint32_t v)
{
    char b[12];
    int i = 0;
    if (v == 0U) { putc_('0'); return; }
    while (v) { b[i++] = (char)('0' + v % 10U); v /= 10U; }
    while (i) putc_(b[--i]);
}

static void putf_(float f, int dec)   /* 简易定点打印 */
{
    uint32_t scale = 1U, ip, fp;
    int k;
    if (f < 0.0f) { putc_('-'); f = -f; }
    for (k = 0; k < dec; k++) scale *= 10U;
    ip = (uint32_t)f;
    fp = (uint32_t)((f - (float)ip) * (float)scale + 0.5f);
    if (fp >= scale) { ip++; fp -= scale; }
    putu_(ip);
    if (dec > 0) {
        putc_('.');
        for (k = dec - 1; k >= 0; k--) {
            uint32_t d = fp;
            int j;
            for (j = 0; j < k; j++) d /= 10U;
            putc_((char)('0' + d % 10U));
        }
    }
}

/* ---------------- EPWM ---------------- */
static void bridge_config(uint32_t base, float duty, float phase_ns, uint16_t master)
{
    uint16_t tbprd = (uint16_t)(PWM_CLK / (g_fs * 1000.0f) / 2.0f + 0.5f);
    uint16_t cmpa  = (uint16_t)((1.0f - duty) * tbprd + 0.5f);
    uint16_t db    = (uint16_t)(g_dt * 1e-9f * PWM_CLK + 0.5f);
    uint16_t phs   = (uint16_t)(phase_ns * 1e-9f * PWM_CLK + 0.5f);

    EPWM_setTimeBasePeriod(base, tbprd);
    EPWM_setTimeBaseCounter(base, 0U);
    EPWM_setTimeBaseCounterMode(base, EPWM_COUNTER_MODE_UP_DOWN);
    EPWM_setClockPrescaler(base, EPWM_CLOCK_DIVIDER_1, EPWM_HSCLOCK_DIVIDER_1);
    if (master) {
        EPWM_disablePhaseShiftLoad(base);
        EPWM_enableSyncOutPulseSource(base, EPWM_SYNC_OUT_PULSE_ON_CNTR_ZERO);
    } else {
        EPWM_setPhaseShift(base, phs);
        EPWM_enablePhaseShiftLoad(base);
        EPWM_setCountModeAfterSync(base, EPWM_COUNT_MODE_UP_AFTER_SYNC);
    }
    EPWM_setCounterCompareValue(base, EPWM_COUNTER_COMPARE_A, cmpa);
    EPWM_setCounterCompareShadowLoadMode(base, EPWM_COUNTER_COMPARE_A,
                                         EPWM_COMP_LOAD_ON_CNTR_ZERO);
    EPWM_setActionQualifierAction(base, EPWM_AQ_OUTPUT_A,
                                  EPWM_AQ_OUTPUT_HIGH, EPWM_AQ_OUTPUT_ON_TIMEBASE_UP_CMPA);
    EPWM_setActionQualifierAction(base, EPWM_AQ_OUTPUT_A,
                                  EPWM_AQ_OUTPUT_LOW, EPWM_AQ_OUTPUT_ON_TIMEBASE_DOWN_CMPA);
    EPWM_setDeadBandDelayMode(base, EPWM_DB_RED, true);
    EPWM_setDeadBandDelayMode(base, EPWM_DB_FED, true);
    /* POLSEL=10 -> AHC high-active-complementary: xA=RED(AQ), xB=inverted FED(AQ)
       -> EPWMxA/EPWMxB are strictly complementary with dead band inserted.
       (Was ACTIVE_HIGH/ACTIVE_HIGH = POLSEL=00 = same polarity, NOT complementary) */
    HWREGH(base + EPWM_O_DBCTL) =
        (uint16_t)((HWREGH(base + EPWM_O_DBCTL) & ~EPWM_DBCTL_POLSEL_M) |
                   (2U << EPWM_DBCTL_POLSEL_S));
    EPWM_setRisingEdgeDelayCount(base, db);
    EPWM_setFallingEdgeDelayCount(base, db);
}

/* 全参数重配(串口改参后调用) */
static void pwm_apply_all(void)
{
    float d1 = g_invert ? (1.0f - g_d1) : g_d1;
    float d2 = g_invert ? (1.0f - g_d2) : g_d2;

    SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    bridge_config(EPWM1_BASE, d1, 0.0f, 1U);
    bridge_config(EPWM2_BASE, d2, g_phi, 0U);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
}

/* 软启动: 占空比从10%坡到目标, 10级x20ms */
static void soft_start(void)
{
    int s;
    float d1t = g_invert ? (1.0f - g_d1) : g_d1;
    float d2t = g_invert ? (1.0f - g_d2) : g_d2;

    puts_("[OL] soft-start...\r\n");
    for (s = 1; s <= 10; s++) {
        float k = 0.1f + 0.09f * (float)s;
        float a = d1t * k, b = d2t * k;
        SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
        bridge_config(EPWM1_BASE, a, 0.0f, 1U);
        bridge_config(EPWM2_BASE, b, g_phi * k, 0U);
        SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
        DEVICE_DELAY_US(20000);
    }
    pwm_apply_all();
}

/* ---------------- 串口命令解析(主循环轮询) ---------------- */
static char rx_line[32];
static uint8_t rx_len = 0U;

static void handle_line(char *ln)
{
    float v = 0.0f;
    int i = 1;
    uint8_t ok = 0U;

    if (ln[1] == ' ') i = 2;
    for (; ln[i] >= '0' && ln[i] <= '9'; i++) {
        v = v * 10.0f + (float)(ln[i] - '0');
        ok = 1U;
    }
    if (ln[i] == '.') {
        float sc = 0.1f;
        i++;
        for (; ln[i] >= '0' && ln[i] <= '9'; i++) { v += sc * (float)(ln[i] - '0'); sc *= 0.1f; }
    }
    switch (ln[0] | 0x20U) {
    case 'f': if (ok && v >= 30.0f && v <= 500.0f) { g_fs = v;  g_seq++; pwm_apply_all(); puts_("[OL] OK fs=");    putf_(g_fs, 1);  puts_(" kHz\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 'a': if (ok && v > 0.5f && v < 99.5f)     { g_d1 = v * 0.01f; g_seq++; pwm_apply_all(); puts_("[OL] OK D1=");    putf_(g_d1 * 100.0f, 2); puts_(" %\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 'b': if (ok && v > 0.5f && v < 99.5f)     { g_d2 = v * 0.01f; g_seq++; pwm_apply_all(); puts_("[OL] OK D2=");    putf_(g_d2 * 100.0f, 2); puts_(" %\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 'p': if (ok && v >= 0.0f && v < 20000.0f) { g_phi = v; g_seq++; pwm_apply_all(); puts_("[OL] OK phi=");   putf_(g_phi, 0);  puts_(" ns\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 't': if (ok && v >= 20.0f && v < 2000.0f) { g_dt = v;  g_seq++; pwm_apply_all(); puts_("[OL] OK dt=");    putf_(g_dt, 0);   puts_(" ns\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 'x': g_invert ^= 1U; g_seq++; pwm_apply_all();
              puts_("[OL] invert="); putu_(g_invert); puts_("\r\n"); break;
    case 'r': soft_start(); break;
    case 's': puts_("[OL] ST fs=");  putf_(g_fs, 1);
              puts_(" D1=");   putf_(g_d1 * 100.0f, 2);
              puts_(" D2=");   putf_(g_d2 * 100.0f, 2);
              puts_(" phi=");  putf_(g_phi, 0);
              puts_(" dt=");   putf_(g_dt, 0);
              puts_(" inv=");  putu_(g_invert);
              puts_(" seq=");  putu_(g_seq);
              puts_("\r\n"); break;
    default: break;
    }
}

static void poll_uart(void)
{
    while (SCI_getRxFIFOStatus(SCIA_BASE) != SCI_FIFO_RX0) {
        char c = (char)SCI_readCharBlockingFIFO(SCIA_BASE);
        if (c == '\n' || c == '\r') {
            if (rx_len > 0U) { rx_line[rx_len] = 0; handle_line(rx_line); rx_len = 0U; }
        } else if (rx_len < sizeof(rx_line) - 1U) {
            rx_line[rx_len++] = c;
        } else {
            rx_len = 0U;
        }
    }
}

/* ---------------- main ---------------- */
void main(void)
{
    uint32_t t_last = 0U, t_now;

    Device_init();
    Device_initGPIO();
    Interrupt_initModule();
    Interrupt_initVectorTable();

    /* ===== EPWM先行: 就算串口卡死也不挡发波 ===== */
    GPIO_setPinConfig(GPIO_0_EPWM1_A);
    GPIO_setPinConfig(GPIO_1_EPWM1_B);
    GPIO_setPinConfig(GPIO_2_EPWM2_A);
    GPIO_setPinConfig(GPIO_3_EPWM2_B);
    GPIO_setPadConfig(0U, GPIO_PIN_TYPE_STD);
    GPIO_setPadConfig(1U, GPIO_PIN_TYPE_STD);
    GPIO_setPadConfig(2U, GPIO_PIN_TYPE_STD);
    GPIO_setPadConfig(3U, GPIO_PIN_TYPE_STD);

    SysCtl_disablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM1);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_EPWM2);
    SysCtl_enablePeripheral(SYSCTL_PERIPH_CLK_TBCLKSYNC);

    scia_init();

    puts_("\r\n=== F28P55x 300W Open-Loop (2x half-bridge, 150MHz) ===\r\n");
    puts_("[OL] default: fs="); putf_(g_fs, 1);
    puts_(" D1="); putf_(g_d1 * 100.0f, 2);
    puts_(" D2="); putf_(g_d2 * 100.0f, 2);
    puts_(" phi="); putf_(g_phi, 0);
    puts_(" dt="); putf_(g_dt, 0);
    puts_(" ns\r\n");
    puts_("[OL] cmds: F<kHz> A<D1%> B<D2%> P<phi_ns> T<dt_ns> X R S\r\n");

    soft_start();
    puts_("[OL] RUN\r\n");

    EINT;
    ERTM;

    while (1) {
        poll_uart();
        g_hb++;
        t_now = g_hb;
        if (t_now - t_last > 8000000U) {          /* ~1s状态行 */
            t_last = t_now;
            puts_("[OL] fs="); putf_(g_fs, 1);
            puts_(" D1="); putf_(g_d1 * 100.0f, 2);
            puts_(" D2="); putf_(g_d2 * 100.0f, 2);
            puts_(" phi="); putf_(g_phi, 0);
            puts_(" seq="); putu_(g_seq);
            puts_("\r\n");
        }
    }
}
