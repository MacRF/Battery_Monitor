/*
 * Battery Monitor WT32-ETH01 - 3-Point Calibration MAX 13.2 Volt PERICOLO GPIO >3.3V
 * Con calibrazione a 3 punti per correggere non-linearità del partitore
 */

 // ═══════════════════════════════════════════════════════════════
// 🌐 CONFIGURAZIONE IP STATICO
// ═══════════════════════════════════════════════════════════════
IPAddress local_ip(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns1(8, 8, 8, 8);
IPAddress dns2(8, 8, 4, 4);

// ═══════════════════════════════════════════════════════════════
// 🔧 CONFIGURAZIONE HARDWARE
// ═══════════════════════════════════════════════════════════════
#define ETH_PHY_POWER   16
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_PHY_ADDR    1
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN

#define BATTERY_PIN     36
#define NUM_SAMPLES     50
#define R1              30000.0
#define R2              10000.0
#define VREF            3.3
#define ADC_RESOLUTION  4095.0

// ═══════════════════════════════════════════════════════════════
// 🔋 CALIBRAZIONE A 3 PUNTI
// ═══════════════════════════════════════════════════════════════
struct CalibrationPoint {
  float measured;  // Valore letto dal dispositivo
  float actual;    // Valore reale misurato con multimetro
};

// Tre punti di calibrazione: basso, medio, alto
CalibrationPoint cal_low    = {10.48, 11.1};   // Punto basso
CalibrationPoint cal_mid    = {11.84, 12.0};   // Punto medio
CalibrationPoint cal_high   = {12.50, 12.4};   // Punto alto

bool calibration_enabled = false;

// Parametri batteria
float BATTERY_MIN = 10.5;
float BATTERY_MAX = 13.8;

// Media mobile
#define MOVING_AVG_SIZE 10
float voltage_history[MOVING_AVG_SIZE];
int history_index = 0;
int history_count = 0;

WebServer server(80);
Preferences preferences;
bool eth_connected = false;
bool eth_started = false;
