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
#include "ai_policy_300w.h"
#include "ai_model/tvmgen_default.h"   /* 300W板DBN策略网络(header-only) */

/* ===================== 工况参数(上电默认, 可串口在线改) ===================== */
#define OL_FS_KHZ   275.0f     /* 开关频率 kHz (300W LUT: 48->24V/4A点) */
#define OL_D1       0.3886f    /* 桥A占空比(EPWM1A高电平占比) */
#define OL_D2       0.5000f    /* 桥B占空比 */
#define OL_PHI_NS   200.0f     /* 桥B相对桥A移相 ns */
#define OL_DT_NS    100.0f     /* 死区 ns */
/* =========================================================================== */


/* ===================== AI 开机配置(改这里就行) =====================
 * AI_BOOT_ENABLE : 1 = 上电自动按下面的工况跑 AI; 0 = 保持 OL 默认工况
 * AI_USE_INT8    : 1 = 用 int8 量化网络; 0 = 用浮点 32-32 网络
 * AI_BOOT_VIN_CV / AI_BOOT_VREF_CV : 输入/目标电压, 单位 cV (2400 = 24.00 V)
 * AI_BOOT_IO_DA  : 负载电流, 单位 0.1 A (10 = 1.0 A)
 * 改完 Ctrl+S -> Ctrl+B -> F11 烧录, 上电即按此工况运行
 * =================================================================== */
#define AI_BOOT_ENABLE   1U
#define AI_USE_INT8      1U
#define AI_BOOT_VIN_CV   3600U
#define AI_BOOT_VREF_CV  2400U
#define AI_BOOT_IO_DA    10U


/* ===================== Vout 闭环 (柳州版逻辑) =====================
 * 标定: VOUT 采样分压 392k/10.5k -> K = 38.33 (原理图计算, 固定值)
 * 逻辑: AI 前馈(实测Vin驱动, 漂移>0.8V/200ms 重推) + PI 只修 D1 小残差
 * ================================================================== */
static float   g_vref   = 24.0f;
static float   g_d1_ff  = 0.0f;
static float   g_vout_k = 38.33f;     /* 392k/10.5k+1 */
static float   g_pi_i   = 0.0f;
static uint8_t g_pi_on  = 0U;
static float   g_vin_anchor = 0.0f;   /* 上次重推时的实测 Vin */
static uint32_t g_drift_ms = 0U;
#define PI_KP        0.0040f
#define PI_KI        0.000080f
#define PI_TRIM_MAX  0.030f           /* PI 只许修 ±3% */
#define PI_TICK      800000U          /* 主循环计数 ~100ms 一拍 */

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

static void puthex_(uint32_t v)
{
    const char *hx = "0123456789ABCDEF";
    int i;
    for (i = 3; i >= 0; i--) putc_(hx[(v >> (i * 4)) & 0xFU]);
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
/* ============================================================================
 * AI: DBN策略网络 -> 工况配方 (300W板)
 * 差异点核对(与柳州板/已验证开环版保持一致的地方):
 *   1) 主频 150MHz -> TBCLK 150MHz, fs = 150MHz/(2*TBPRD)      [bridge_config内]
 *   2) 死区极性 POLSEL=10(AHC互补)                             [bridge_config内, 已实测]
 *   3) CMPA = (1-D)*TBPRD, D=上管占空比                        [bridge_config内, 已实测]
 *   4) 死区伏秒修正公式与AI 300瓦计算器完全一致:
 *        D1 = Vref/Vin*(D2 + b*δ) + 2δ,  δ = td*fs, b = b_of_phi(phi*fs)
 * ==========================================================================*/
static void soft_start(void);   /* 前置声明: ai300_apply会调用 */

static float b_of_phi_c(float phi_duty)
{
    float p = (phi_duty - 0.05f) / 0.40f;
    if (p < 0.0f) { p = 0.0f; }
    if (p > 1.0f) { p = 1.0f; }
    return 1.33f * p;
}


/* ===== int8 模型 (tvmc 生成, CPU 推理) ===== */
#define AI8_IN_SCALE  0.04198694f
#define AI8_IN_ZP     43.0f
#define AI8_OUT_SCALE 0.046132874f
#define AI8_OUT_ZP    67.0f
static float ai8_in[4];
static float ai8_out[4];

static void  adc_poll(void);
static void  adc_poll_avg(void);
static float adc_volts(uint16_t k);

static void ai300_int8_run(float vin, float vref, float io,
                           float *fs, float *d1, float *d2, float *phi)
{
    float x4[4], y[4];
    int   j;
    struct tvmgen_default_inputs  ins;
    struct tvmgen_default_outputs outs;

    x4[0] = vin; x4[1] = vref; x4[2] = io; x4[3] = vref / vin;
    for (j = 0; j < 4; j++) {                       /* z-score 归一化 */
        ai8_in[j] = (x4[j] - AI300_MX[j]) / AI300_SX[j];
    }
    ins.input_1  = (void *)ai8_in;
    outs.output  = (void *)ai8_out;
    tvmgen_default_run(&ins, &outs);

    for (j = 0; j < 4; j++) {                       /* 反归一化 */
        y[j] = ai8_out[j] * AI300_SY[j] + AI300_MY[j];
    }
    *fs = y[0]; *d1 = y[1]; *d2 = y[2]; *phi = y[3];
}

/* ===================== ADC 多路采样 =====================
 * 映射规律: ADCINn -> GPIO(222+n)   (A6=GPIO228 与 TI LaunchPad 例程一致)
 *   SOC0  A2 / GPIO224  -> Vin  采样
 *   SOC1  A3 / GPIO225  -> Vout 采样
 *   SOC2  A4 / GPIO226  -> Iin  采样
 *   SOC3  A5 / GPIO227  -> Iout 采样
 *   SOC4  A6 / GPIO228  -> 备用/温度
 * 每路都留了标定系数, 按实际分压比/霍尔灵敏度改 ADC_K_xx 即可
 * ======================================================== */
#define ADC_N_CH       5U
#define ADC_SAMPLE_WIN 64U

static const uint16_t adc_gpio[ADC_N_CH] = {224U, 225U, 226U, 227U, 228U};
static uint16_t adc_raw[ADC_N_CH];

/* raw(0..4095) -> 引脚电压(0..3.3V) -> 实际量(乘分压比) */
static void adc_init(void)
{
    uint16_t k;
    ADC_setVREF(ADCA_BASE, ADC_REFERENCE_INTERNAL, ADC_REFERENCE_3_3V);
    ADC_setPrescaler(ADCA_BASE, ADC_CLK_DIV_4_0);
    ADC_setInterruptPulseMode(ADCA_BASE, ADC_PULSE_END_OF_CONV);
    ADC_enableConverter(ADCA_BASE);
    DEVICE_DELAY_US(1000);

    for (k = 0U; k < ADC_N_CH; k++) {
        ADC_setupSOC(ADCA_BASE, (ADC_SOCNumber)k, ADC_TRIGGER_SW_ONLY,
                     (ADC_Channel)(2 + k), ADC_SAMPLE_WIN);
        GPIO_setAnalogMode(adc_gpio[k], GPIO_ANALOG_ENABLED);
    }
    ADC_setInterruptSource(ADCA_BASE, ADC_INT_NUMBER1, ADC_SOC_NUMBER4);
    ADC_enableInterrupt(ADCA_BASE, ADC_INT_NUMBER1);
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
}

/* 一次触发采全部 5 路, 结果放 adc_raw[] */
static void adc_poll(void)
{
    uint16_t k, guard = 0U;
    ADC_forceMultipleSOC(ADCA_BASE, 0x001FU);
    while (ADC_getInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1) == false) {
        if (++guard > 60000U) { break; }
    }
    ADC_clearInterruptStatus(ADCA_BASE, ADC_INT_NUMBER1);
    for (k = 0U; k < ADC_N_CH; k++) {
        adc_raw[k] = (uint16_t)ADC_readResult(ADCARESULT_BASE, (ADC_SOCNumber)k);
    }
}

/* 多次采样求平均, 抗开关噪声 */
static void adc_poll_avg(void)
{
    uint16_t k, n;
    uint32_t acc[ADC_N_CH] = {0, 0, 0, 0, 0};
    for (n = 0U; n < 16U; n++) {
        adc_poll();
        for (k = 0U; k < ADC_N_CH; k++) {
            acc[k] += adc_raw[k];
        }
        DEVICE_DELAY_US(10);        /* 错开采样相位, 避免总是踩在开关沿 */
    }
    for (k = 0U; k < ADC_N_CH; k++) {
        adc_raw[k] = (uint16_t)(acc[k] / 16U);
    }
}

static float adc_volts(uint16_t k)
{
    return (float)adc_raw[k] * 3.3f / 4095.0f;
}


/* 闭环一拍: Vout 中值滤波 -> (Vin漂移则重推AI) -> PI 修 D1 */
static void vout_loop_tick(void)
{
    float vout, vin_meas, err, trim, d1;

    /* --- 中值滤波: 连采 15 次取中值 (抗开关尖峰) --- */
    {
        uint16_t buf[15];
        uint8_t a, b2, t;
        for (t = 0U; t < 15U; t++) {
            adc_poll();
            buf[t] = adc_raw[1];               /* A3 = VOUT */
            DEVICE_DELAY_US(5);
        }
        for (a = 0U; a < 15U - 1U; a++) {      /* 插入排序 */
            for (b2 = 0U; b2 < 15U - 1U - a; b2++) {
                if (buf[b2] > buf[b2 + 1]) {
                    uint16_t tmp = buf[b2]; buf[b2] = buf[b2 + 1]; buf[b2 + 1] = tmp;
                }
            }
        }
        vout = (float)buf[7] * 3.3f / 4095.0f * g_vout_k;
    }

    if (!g_pi_on) { return; }

    /* --- Vin 漂移检测: >0.8V 持续 200ms(2拍) -> 重推 AI 配方 --- */
    vin_meas = adc_volts(0) * g_vout_k;        /* A2 = VIN, 同一分压比 */
    if (g_vin_anchor > 5.0f) {
        if ((vin_meas - g_vin_anchor) > 0.8f || (g_vin_anchor - vin_meas) > 0.8f) {
            g_drift_ms += 100U;
            if (g_drift_ms >= 200U) {
                float fs8, d18, d28, ph8;
                ai300_int8_run(vin_meas, g_vref, 1.0f, &fs8, &d18, &d28, &ph8);
                ai300_apply(vin_meas, g_vref, 1.0f);   /* 重算配方+死区修正+下发 */
                g_vin_anchor = vin_meas;
                g_drift_ms = 0U;
                puts_("[AIr] Vin drift -> re-infer: ");
                putf_(vin_meas, 1); puts_("V\r\n");
                return;                        /* 本拍让软启动走完 */
            }
        } else {
            g_drift_ms = 0U;
        }
    } else {
        g_vin_anchor = vin_meas;
    }

    /* --- PI 只修小残差 --- */
    err = g_vref - vout;
    g_pi_i += err;
    if (g_pi_i >  150.0f) { g_pi_i =  150.0f; }
    if (g_pi_i < -150.0f) { g_pi_i = -150.0f; }
    trim = PI_KP * err + PI_KI * g_pi_i;
    if (trim >  PI_TRIM_MAX) { trim =  PI_TRIM_MAX; }
    if (trim < -PI_TRIM_MAX) { trim = -PI_TRIM_MAX; }
    d1 = g_d1_ff + trim;
    if (d1 < 0.05f) { d1 = 0.05f; }
    if (d1 > 0.95f) { d1 = 0.95f; }
    g_d1 = d1;
    pwm_apply_all();
}


static void ai300_apply(float vin, float vref, float io)
{
    float fs, d1_tbl, d2, phi;
    float delta, b, d1, T_ns, phi_max;

    ai300_recipe(vin, vref, io, &fs, &d1_tbl, &d2, &phi);

    /* 死区伏秒修正 (δ = 死区秒 * 开关频率Hz) */
    delta = (g_dt * 1e-9f) * (fs * 1000.0f);
    b     = b_of_phi_c(phi * 1e-9f * fs * 1000.0f);
    d1    = (vref / vin) * (d2 + b * delta) + 2.0f * delta;

    /* 移相不得超过一个周期(否则TBPHS回绕) */
    T_ns    = 1.0e6f / fs;              /* fs单位kHz -> 周期ns */
    phi_max = 0.90f * T_ns;
    if (phi > phi_max) { phi = phi_max; }

    /* 物理限幅 */
    if (d1 < 0.05f) { d1 = 0.05f; }
    if (d1 > 0.95f) { d1 = 0.95f; }
    if (d2 < 0.10f) { d2 = 0.10f; }
    if (d2 > 0.90f) { d2 = 0.90f; }

    g_fs  = fs;
    g_d1  = d1;
    g_d2  = d2;
    g_phi = phi;
    g_seq++;
    g_vref  = vref;
    g_d1_ff = d1;
    g_pi_i  = 0.0f;
    g_pi_on = 1U;

    puts_("[AI] VIN="); putf_(vin, 2);
    puts_(" VREF="); putf_(vref, 2);
    puts_(" IO="); putf_(io, 2);
    puts_(" -> FS="); putf_(fs, 1);
    puts_("kHz D2="); putf_(d2 * 100.0f, 2);
    puts_("% PHI="); putf_(phi, 0);
    puts_("ns\r\n");
    puts_("[AI] D1_TBL="); putf_(d1_tbl * 100.0f, 2);
    puts_("% D1_CORR="); putf_(d1 * 100.0f, 2);
    puts_("% dELTA="); putf_(delta * 100.0f, 2);
    puts_("% B="); putf_(b, 2);
    puts_("\r\n");

    {   /* 上电打印一次 ADC 原始值, 便于确认采样脚已接 */
        adc_poll();
        puts_("[ADC] VIN="); putf_(adc_volts(0), 2); puts_("V VOUT=");
        putf_(adc_volts(1), 2); puts_("V IIN="); putf_(adc_volts(2), 2);
        puts_("V IOUT="); putf_(adc_volts(3), 2); puts_("V AUX=");
        putf_(adc_volts(4), 2); puts_("V\r\n");
    }
    soft_start();
}

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


/* int8 网络: 算配方 -> 死区修正 -> 限幅 -> 下发(与 G 命令的浮点路径等价) */
static void ai300_int8_apply(float vin, float vref, float io)
{
    float fs, d1_tbl, d2, phi, delta, b, d1, T_ns, phi_max;
    ai300_int8_run(vin, vref, io, &fs, &d1_tbl, &d2, &phi);

    delta = (g_dt * 1e-9f) * (fs * 1000.0f);
    b     = b_of_phi_c(phi * 1e-9f * fs * 1000.0f);
    d1    = (vref / vin) * (d2 + b * delta) + 2.0f * delta;

    T_ns    = 1.0e6f / fs;
    phi_max = 0.90f * T_ns;
    if (phi > phi_max) { phi = phi_max; }
    if (d1 < 0.05f) { d1 = 0.05f; }
    if (d1 > 0.95f) { d1 = 0.95f; }
    if (d2 < 0.10f) { d2 = 0.10f; }
    if (d2 > 0.90f) { d2 = 0.90f; }

    g_fs = fs; g_d1 = d1; g_d2 = d2; g_phi = phi; g_seq++;
    g_vref = vref; g_d1_ff = d1; g_pi_i = 0.0f;
    puts_("[AI8-I] VIN="); putf_(vin, 1); puts_(" VREF="); putf_(vref, 1);
    puts_(" IO="); putf_(io, 1);
    puts_(" -> FS="); putf_(fs, 1); puts_("kHz D2="); putf_(d2 * 100.0f, 2);
    puts_("% PHI="); putf_(phi, 0); puts_("ns\r\n");
    soft_start();
}



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
    /* ---- AI: G <Vin_cV> <Vref_cV> <Io_dA>  例: G 3600 2400 10 ---- */
    if ((ln[0] | 0x20U) == 'g')
    {
        int vals[3] = {0, 0, 0};
        int n = 0, j = 1, got = 0;
        while (ln[j] != 0 && n < 3)
        {
            if (ln[j] >= '0' && ln[j] <= '9')
            {
                vals[n] = vals[n] * 10 + (ln[j] - '0');
                got = 1;
            }
            else if (got)
            {
                n++;
                got = 0;
            }
            j++;
        }
        if (got) { n++; }

        if (n == 3 &&
            vals[0] >= 2400 && vals[0] <= 4800 &&
            vals[1] >= 2400 && vals[1] <= 4800 &&
            vals[2] >= 0    && vals[2] <= 150)
        {
            ai300_apply((float)vals[0] * 0.01f,
                        (float)vals[1] * 0.01f,
                        (float)vals[2] * 0.1f);
        }
        else
        {
            puts_("[AI] ERR 用法: G <Vin_cV> <Vref_cV> <Io_dA>  例 G 3600 2400 10\r\n");
        }
        return;
    }

    /* ---- int8 模型: K <Vin_cV> <Vref_cV> <Io_dA> ---- */
    if ((ln[0] | 0x20U) == 'k')
    {
        int vals[3] = {0, 0, 0};
        int n = 0, j = 1, got = 0;
        while (ln[j] != 0 && n < 3)
        {
            if (ln[j] >= '0' && ln[j] <= '9') { vals[n] = vals[n] * 10 + (ln[j] - '0'); got = 1; }
            else if (got) { n++; got = 0; }
            j++;
        }
        if (got) { n++; }
        if (n == 3)
        {
            float vin = (float)vals[0] * 0.01f, vref = (float)vals[1] * 0.01f,
                  io = (float)vals[2] * 0.1f;
            float fs8, d18, d28, ph8, fsf, d1f, d2f, phf;
            ai300_int8_run(vin, vref, io, &fs8, &d18, &d28, &ph8);
            ai300_recipe(vin, vref, io, &fsf, &d1f, &d2f, &phf);
            puts_("[AI8] VIN="); putf_(vin, 1); puts_(" VREF="); putf_(vref, 1);
            puts_(" IO="); putf_(io, 1);
            puts_(" | i8 FS="); putf_(fs8, 1); puts_(" D1="); putf_(d18 * 100.0f, 2);
            puts_(" D2="); putf_(d28 * 100.0f, 2); puts_(" PHI="); putf_(ph8, 0);
            puts_(" | f32 FS="); putf_(fsf, 1); puts_(" D1="); putf_(d1f * 100.0f, 2);
            puts_(" D2="); putf_(d2f * 100.0f, 2); puts_(" PHI="); putf_(phf, 0);
            puts_("\r\n");
        }
        else
        {
            puts_("[AI8] usage: K <Vin_cV> <Vref_cV> <Io_dA>\r\n");
        }
        return;
    }

    /* ---- M: 读 5 路 ADC (A2..A6) ---- */
    if ((ln[0] | 0x20U) == 'm')
    {
        adc_poll();
        puts_("[ADC] VIN=");  putu_(adc_raw[0]); puts_("("); putf_(adc_volts(0), 3);
        puts_("V) VOUT="); putu_(adc_raw[1]); puts_("("); putf_(adc_volts(1), 3);
        puts_("V) IIN=");  putu_(adc_raw[2]); puts_("("); putf_(adc_volts(2), 3);
        puts_("V) IOUT="); putu_(adc_raw[3]); puts_("("); putf_(adc_volts(3), 3);
        puts_("V) AUX=");  putu_(adc_raw[4]); puts_("("); putf_(adc_volts(4), 3);
        puts_("V)\r\n");
        return;
    }

    /* ---- C <实际输出电压cV>: 标定分压比, 标定后闭环使能 ---- */
    if ((ln[0] | 0x20U) == 'c' && ok)
    {
        float pin;
        adc_poll_avg();
        pin = adc_volts(1);
        if (pin > 0.05f)
        {
            g_vout_k = (v * 0.01f) / pin;
            g_pi_i   = 0.0f;
            g_pi_on  = 1U;
            puts_("[CAL] Vout="); putf_(v * 0.01f, 2);
            puts_("V pin="); putf_(pin, 3);
            puts_("V K="); putf_(g_vout_k, 2);
            puts_("  closed-loop ON\r\n");
        }
        else
        {
            puts_("[CAL] pin too low, check VOUT sense wire\r\n");
        }
        return;
    }

    /* ---- L: 连续打印 5 路 3 秒 ---- */
    if ((ln[0] | 0x20U) == 'l')
    {
        uint16_t t;
        for (t = 0U; t < 30U; t++)
        {
            adc_poll();
            puts_("[L] A2="); putf_(adc_volts(0), 2);
            puts_(" A3="); putf_(adc_volts(1), 2);
            puts_(" A4="); putf_(adc_volts(2), 2);
            puts_(" A5="); putf_(adc_volts(3), 2);
            puts_(" A6="); putf_(adc_volts(4), 2);
            puts_("\r\n");
            DEVICE_DELAY_US(100000);
        }
        return;
    }

    switch (ln[0] | 0x20U) {
    case 'f': if (ok && v >= 30.0f && v <= 500.0f) { g_fs = v;  g_seq++; pwm_apply_all(); puts_("[OL] OK fs=");    putf_(g_fs, 1);  puts_(" kHz\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 'a': if (ok && v > 0.5f && v < 99.5f)     { g_d1 = v * 0.01f; g_seq++; pwm_apply_all(); puts_("[OL] OK D1=");    putf_(g_d1 * 100.0f, 2); puts_(" %\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 'b': if (ok && v > 0.5f && v < 99.5f)     { g_d2 = v * 0.01f; g_seq++; pwm_apply_all(); puts_("[OL] OK D2=");    putf_(g_d2 * 100.0f, 2); puts_(" %\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 'p': if (ok && v >= 0.0f && v < 20000.0f) { g_phi = v; g_seq++; pwm_apply_all(); puts_("[OL] OK phi=");   putf_(g_phi, 0);  puts_(" ns\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 't': if (ok && v >= 20.0f && v < 2000.0f) { g_dt = v;  g_seq++; pwm_apply_all(); puts_("[OL] OK dt=");    putf_(g_dt, 0);   puts_(" ns\r\n"); } else puts_("[OL] ERR\r\n"); break;
    case 'x': g_invert ^= 1U; g_seq++; pwm_apply_all();
              puts_("[OL] invert="); putu_(g_invert); puts_("\r\n"); break;
    case 'd': {
        uint16_t dbctl1 = HWREGH(EPWM1_BASE + EPWM_O_DBCTL);
        uint16_t dbctl2 = HWREGH(EPWM2_BASE + EPWM_O_DBCTL);
        puts_("[OL] EPWM1 DBCTL=0x"); puthex_(dbctl1);
        puts_(" POLSEL="); putu_((dbctl1 >> 2) & 3U);
        puts_(" DBRED="); putu_(HWREGH(EPWM1_BASE + EPWM_O_DBRED));
        puts_(" DBFED="); putu_(HWREGH(EPWM1_BASE + EPWM_O_DBFED));
        puts_(" TBPRD="); putu_(HWREGH(EPWM1_BASE + EPWM_O_TBPRD));
        puts_(" CMPA="); putu_(HWREGH(EPWM1_BASE + EPWM_O_CMPA + 1U));
        puts_("\r\n");
        puts_("[OL] EPWM2 DBCTL=0x"); puthex_(dbctl2);
        puts_(" POLSEL="); putu_((dbctl2 >> 2) & 3U);
        puts_(" DBRED="); putu_(HWREGH(EPWM2_BASE + EPWM_O_DBRED));
        puts_(" DBFED="); putu_(HWREGH(EPWM2_BASE + EPWM_O_DBFED));
        puts_(" TBPRD="); putu_(HWREGH(EPWM2_BASE + EPWM_O_TBPRD));
        puts_(" CMPA="); putu_(HWREGH(EPWM2_BASE + EPWM_O_CMPA + 1U));
        puts_(" TBPHS="); putu_(HWREGH(EPWM2_BASE + EPWM_O_TBPHS + 1U));
        puts_("\r\n");
        break;
    }
    case 'e': {
        puts_("[OL] CMPA32="); putu_(HWREG(EPWM1_BASE + EPWM_O_CMPA));
        puts_(" CMPA16L="); putu_(HWREGH(EPWM1_BASE + EPWM_O_CMPA + 1U));
        puts_(" CMPA16H="); putu_(HWREGH(EPWM1_BASE + EPWM_O_CMPA + 1U));
        puts_(" PH32="); putu_(HWREG(EPWM2_BASE + EPWM_O_TBPHS));
        puts_(" PH16L="); putu_(HWREGH(EPWM2_BASE + EPWM_O_TBPHS + 1U));
        puts_(" PH16H="); putu_(HWREGH(EPWM2_BASE + EPWM_O_TBPHS + 1U));
        puts_("\r\n");
        puts_("[OL] CMPA2_32="); putu_(HWREG(EPWM2_BASE + EPWM_O_CMPA));
        puts_(" TBPRD="); putu_(HWREGH(EPWM1_BASE + EPWM_O_TBPRD));
        puts_("\r\n");
        break;
    }
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
    adc_init();

    puts_("\r\n=== F28P55x 300W Open-Loop (2x half-bridge, 150MHz) ===\r\n");
    puts_("[OL] default: fs="); putf_(g_fs, 1);
    puts_(" D1="); putf_(g_d1 * 100.0f, 2);
    puts_(" D2="); putf_(g_d2 * 100.0f, 2);
    puts_(" phi="); putf_(g_phi, 0);
    puts_(" dt="); putf_(g_dt, 0);
    puts_(" ns\r\n");
    puts_("[OL] cmds: F<kHz> A<D1%> B<D2%> P<phi_ns> T<dt_ns> X R S D E\r\n");
    puts_("[AI] G <Vin_cV> <Vref_cV> <Io_dA>  例: G 3600 2400 10 -> DBN算配方并应用\r\n");

    soft_start();
#if AI_BOOT_ENABLE
    puts_("[AI] boot AI: ");
    putf_((float)AI_BOOT_VIN_CV * 0.01f, 1); puts_("V -> ");
    putf_((float)AI_BOOT_VREF_CV * 0.01f, 1); puts_("V / ");
    putf_((float)AI_BOOT_IO_DA * 0.1f, 1); puts_("A (");
#if AI_USE_INT8
    puts_("int8)\r\n");
    ai300_int8_apply((float)AI_BOOT_VIN_CV * 0.01f, (float)AI_BOOT_VREF_CV * 0.01f,
                     (float)AI_BOOT_IO_DA * 0.1f);
#else
    puts_("float)\r\n");
    ai300_apply((float)AI_BOOT_VIN_CV * 0.01f, (float)AI_BOOT_VREF_CV * 0.01f,
                (float)AI_BOOT_IO_DA * 0.1f);
#endif
#endif

    EINT;
    ERTM;

    while (1) {
        poll_uart();
        g_hb++;
        if ((g_hb % PI_TICK) == 0U) { vout_loop_tick(); }
        t_now = g_hb;
        if (t_now - t_last > 8000000U) {          /* ~1s状态行 */
            t_last = t_now;
            puts_("[OL] fs="); putf_(g_fs, 1);
            puts_(" D1="); putf_(g_d1 * 100.0f, 2);
            puts_(" D2="); putf_(g_d2 * 100.0f, 2);
            puts_(" phi="); putf_(g_phi, 0);
            puts_(" seq="); putu_(g_seq);
            adc_poll();
            puts_(" VIN="); putf_(adc_volts(0), 2);
            puts_("V VOUT="); putf_(adc_volts(1), 2);
            puts_("V IOUT="); putf_(adc_volts(3), 2); puts_("V");
            puts_("\r\n");
        }
    }
}
