/*
 * Battery Monitor WT32-ETH01 - 3-Point Calibration
 * Con calibrazione a 3 punti per correggere non-linearità del partitore
 */

#include <ETH.h>
#include <WebServer.h>
#include <Preferences.h>
#include <NetworkEvents.h>

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

// ═══════════════════════════════════════════════════════════════
// FUNZIONI CALIBRAZIONE
// ═══════════════════════════════════════════════════════════════

void loadCalibration() {
  preferences.begin("calibration", true);
  
  calibration_enabled = preferences.getBool("enabled", false);
  
  if(calibration_enabled) {
    cal_low.measured = preferences.getFloat("low_m", 10.48);
    cal_low.actual = preferences.getFloat("low_a", 11.1);
    
    cal_mid.measured = preferences.getFloat("mid_m", 11.84);
    cal_mid.actual = preferences.getFloat("mid_a", 12.0);
    
    cal_high.measured = preferences.getFloat("high_m", 12.50);
    cal_high.actual = preferences.getFloat("high_a", 12.4);
    
    Serial.println("🔧 Calibrazione a 3 punti caricata:");
    Serial.printf("   Basso:  %.2fV → %.2fV\n", cal_low.measured, cal_low.actual);
    Serial.printf("   Medio:  %.2fV → %.2fV\n", cal_mid.measured, cal_mid.actual);
    Serial.printf("   Alto:   %.2fV → %.2fV\n", cal_high.measured, cal_high.actual);
  } else {
    Serial.println("⚠️  Calibrazione disabilitata - usando valori grezzi");
  }
  
  preferences.end();
}

void saveCalibration(float low_m, float low_a, float mid_m, float mid_a, float high_m, float high_a, bool enabled) {
  preferences.begin("calibration", false);
  
  preferences.putFloat("low_m", low_m);
  preferences.putFloat("low_a", low_a);
  preferences.putFloat("mid_m", mid_m);
  preferences.putFloat("mid_a", mid_a);
  preferences.putFloat("high_m", high_m);
  preferences.putFloat("high_a", high_a);
  preferences.putBool("enabled", enabled);
  
  preferences.end();
  
  cal_low.measured = low_m;
  cal_low.actual = low_a;
  cal_mid.measured = mid_m;
  cal_mid.actual = mid_a;
  cal_high.measured = high_m;
  cal_high.actual = high_a;
  calibration_enabled = enabled;
  
  Serial.println("✓ Calibrazione salvata");
}

// Interpolazione lineare tra due punti
float interpolate(float x, float x1, float y1, float x2, float y2) {
  if(x2 == x1) return y1;  // Evita divisione per zero
  return y1 + (x - x1) * (y2 - y1) / (x2 - x1);
}

// Applica calibrazione a 3 punti
float applyCalibratio(float raw_voltage) {
  if(!calibration_enabled) {
    return raw_voltage;  // Nessuna calibrazione
  }
  
  // Determina quale intervallo usare
  if(raw_voltage <= cal_mid.measured) {
    // Tra punto basso e medio
    return interpolate(raw_voltage, 
                      cal_low.measured, cal_low.actual,
                      cal_mid.measured, cal_mid.actual);
  } else {
    // Tra punto medio e alto
    return interpolate(raw_voltage,
                      cal_mid.measured, cal_mid.actual,
                      cal_high.measured, cal_high.actual);
  }
}

void loadBatteryParams() {
  preferences.begin("battery", true);
  BATTERY_MIN = preferences.getFloat("min", 10.5);
  BATTERY_MAX = preferences.getFloat("max", 13.8);
  preferences.end();
  
  Serial.println("🔋 Parametri batteria:");
  Serial.printf("   Min: %.2fV, Max: %.2fV\n", BATTERY_MIN, BATTERY_MAX);
}

void saveBatteryParams(float min_v, float max_v) {
  preferences.begin("battery", false);
  preferences.putFloat("min", min_v);
  preferences.putFloat("max", max_v);
  preferences.end();
  
  BATTERY_MIN = min_v;
  BATTERY_MAX = max_v;
  
  Serial.printf("✓ Parametri salvati: Min=%.2fV, Max=%.2fV\n", min_v, max_v);
}

void loadNetworkConfig() {
  preferences.begin("network", true);
  
  if(preferences.getBool("configured", false)) {
    local_ip = IPAddress(
      preferences.getUChar("ip1", local_ip[0]),
      preferences.getUChar("ip2", local_ip[1]),
      preferences.getUChar("ip3", local_ip[2]),
      preferences.getUChar("ip4", local_ip[3])
    );
    
    gateway = IPAddress(
      preferences.getUChar("gw1", gateway[0]),
      preferences.getUChar("gw2", gateway[1]),
      preferences.getUChar("gw3", gateway[2]),
      preferences.getUChar("gw4", gateway[3])
    );
    
    subnet = IPAddress(
      preferences.getUChar("sn1", subnet[0]),
      preferences.getUChar("sn2", subnet[1]),
      preferences.getUChar("sn3", subnet[2]),
      preferences.getUChar("sn4", subnet[3])
    );
    
    Serial.println("✓ IP caricato da memoria");
  }
  
  preferences.end();
}

void saveNetworkConfig(IPAddress ip, IPAddress gw, IPAddress sn) {
  preferences.begin("network", false);
  
  preferences.putUChar("ip1", ip[0]);
  preferences.putUChar("ip2", ip[1]);
  preferences.putUChar("ip3", ip[2]);
  preferences.putUChar("ip4", ip[3]);
  
  preferences.putUChar("gw1", gw[0]);
  preferences.putUChar("gw2", gw[1]);
  preferences.putUChar("gw3", gw[2]);
  preferences.putUChar("gw4", gw[3]);
  
  preferences.putUChar("sn1", sn[0]);
  preferences.putUChar("sn2", sn[1]);
  preferences.putUChar("sn3", sn[2]);
  preferences.putUChar("sn4", sn[3]);
  
  preferences.putBool("configured", true);
  preferences.end();
}

// ═══════════════════════════════════════════════════════════════
// LETTURA BATTERIA
// ═══════════════════════════════════════════════════════════════

float readBatteryVoltageRaw() {
  uint32_t adc_sum = 0;
  
  for(int i = 0; i < NUM_SAMPLES; i++) {
    adc_sum += analogRead(BATTERY_PIN);
    delayMicroseconds(100);
  }
  
  float adc_avg = adc_sum / (float)NUM_SAMPLES;
  float v_measured = (adc_avg / ADC_RESOLUTION) * VREF;
  float v_battery = v_measured * ((R1 + R2) / R2);
  
  // Applica calibrazione a 3 punti
  return applyCalibratio(v_battery);
}

float readBatteryVoltage() {
  float raw_voltage = readBatteryVoltageRaw();
  
  voltage_history[history_index] = raw_voltage;
  history_index = (history_index + 1) % MOVING_AVG_SIZE;
  
  if(history_count < MOVING_AVG_SIZE) {
    history_count++;
  }
  
  float sum = 0;
  for(int i = 0; i < history_count; i++) {
    sum += voltage_history[i];
  }
  
  return sum / history_count;
}

float getBatteryPercentage(float voltage) {
  if(voltage >= BATTERY_MAX) return 100.0;
  if(voltage <= BATTERY_MIN) return 0.0;
  
  float percentage = ((voltage - BATTERY_MIN) / (BATTERY_MAX - BATTERY_MIN)) * 100.0;
  return constrain(percentage, 0.0, 100.0);
}

String getBatteryStatus(float voltage) {
  if(voltage >= 13.2) return "Carica";
  if(voltage >= 12.7) return "Piena";
  if(voltage >= 12.4) return "Buona";
  if(voltage >= 12.0) return "Media";
  if(voltage >= 11.5) return "Bassa";
  if(voltage >= 10.5) return "Critica";
  return "Scarica";
}

// ═══════════════════════════════════════════════════════════════
// WEB HANDLERS
// ═══════════════════════════════════════════════════════════════

void handleRoot() {
  String calStatus = calibration_enabled ? "Abilitata" : "Disabilitata";
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang='it'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Monitor Batteria 12V</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            padding: 40px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 500px;
            width: 100%;
        }
        h1 { color: #333; text-align: center; margin-bottom: 30px; font-size: 28px; }
        .battery-icon {
            width: 200px;
            height: 100px;
            border: 4px solid #333;
            border-radius: 8px;
            margin: 0 auto 30px;
            position: relative;
            background: #f0f0f0;
        }
        .battery-terminal {
            position: absolute;
            right: -15px;
            top: 50%;
            transform: translateY(-50%);
            width: 15px;
            height: 40px;
            background: #333;
            border-radius: 0 4px 4px 0;
        }
        .battery-level {
            position: absolute;
            left: 4px;
            top: 4px;
            bottom: 4px;
            width: 0%;
            background: #44ff44;
            border-radius: 4px;
            transition: width 0.5s ease, background 0.5s ease;
        }
        .info-box {
            background: #f8f9fa;
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 15px;
        }
        .info-row {
            display: flex;
            justify-content: space-between;
            margin-bottom: 15px;
            align-items: center;
        }
        .info-row:last-child { margin-bottom: 0; }
        .label { color: #666; font-size: 14px; font-weight: 500; }
        .value { font-size: 24px; font-weight: bold; color: #333; }
        .voltage { color: #667eea; }
        .percentage { color: #44ff44; }
        .status {
            display: inline-block;
            padding: 8px 16px;
            border-radius: 20px;
            font-size: 14px;
            font-weight: bold;
            color: white;
        }
        .status-charging { background: #44ff44; }
        .status-full { background: #00cc88; }
        .status-good { background: #00aa44; }
        .status-medium { background: #ffaa00; }
        .status-low { background: #ff6600; }
        .status-critical { background: #ff4444; }
        .status-empty { background: #cc0000; }
        .pulse { animation: pulse 2s infinite; }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.6; } }
        .update-time { text-align: center; color: #999; font-size: 12px; margin-top: 10px; }
        .cal-badge {
            text-align: center;
            padding: 8px;
            margin-top: 10px;
            border-radius: 8px;
            font-size: 12px;
            background: #e3f2fd;
            color: #1976d2;
        }
        .btn-group {
            display: grid;
            grid-template-columns: 1fr 1fr 1fr;
            gap: 10px;
            margin-top: 20px;
        }
        .config-btn {
            display: block;
            padding: 12px;
            background: #667eea;
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 13px;
            cursor: pointer;
            text-decoration: none;
            text-align: center;
        }
        .config-btn:hover { background: #5568d3; }
        .config-btn.secondary { background: #95a5a6; }
        .config-btn.secondary:hover { background: #7f8c8d; }
        .config-btn.tertiary { background: #34495e; }
        .config-btn.tertiary:hover { background: #2c3e50; }
    </style>
</head>
<body>
    <div class='container'>
        <h1>🔋 Monitor Batteria 12V</h1>
        <div class='battery-icon'>
            <div class='battery-level' id='batteryLevel'></div>
            <div class='battery-terminal'></div>
        </div>
        <div class='info-box'>
            <div class='info-row'>
                <span class='label'>Tensione:</span>
                <span class='value voltage' id='voltage'>--.-</span>
            </div>
            <div class='info-row'>
                <span class='label'>Carica:</span>
                <span class='value percentage' id='percentage'>--%</span>
            </div>
            <div class='info-row'>
                <span class='label'>Stato:</span>
                <span class='status' id='status'>Connessione...</span>
            </div>
        </div>
        <div class='update-time'>
            <span class='pulse'>●</span> Media mobile su 10 letture
        </div>
        <div class='cal-badge'>
            🔧 Calibrazione: )rawliteral" + calStatus + R"rawliteral(
        </div>
        <div class='btn-group'>
            <a href='/calibration' class='config-btn'>🎯 Calibrazione</a>
            <a href='/settings' class='config-btn secondary'>⚙️ Parametri</a>
            <a href='/config' class='config-btn tertiary'>🌐 Config IP</a>
        </div>
    </div>
    <script>
        function updateBattery() {
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('voltage').textContent = data.voltage.toFixed(2) + ' V';
                    document.getElementById('percentage').textContent = data.percentage.toFixed(0) + '%';
                    const batteryLevel = document.getElementById('batteryLevel');
                    batteryLevel.style.width = data.percentage + '%';
                    if(data.percentage >= 80) batteryLevel.style.background = '#44ff44';
                    else if(data.percentage >= 50) batteryLevel.style.background = '#ffaa00';
                    else if(data.percentage >= 20) batteryLevel.style.background = '#ff6600';
                    else batteryLevel.style.background = '#ff4444';
                    const statusElement = document.getElementById('status');
                    statusElement.textContent = data.status;
                    statusElement.className = 'status';
                    if(data.status === 'Carica') statusElement.classList.add('status-charging');
                    else if(data.status === 'Piena') statusElement.classList.add('status-full');
                    else if(data.status === 'Buona') statusElement.classList.add('status-good');
                    else if(data.status === 'Media') statusElement.classList.add('status-medium');
                    else if(data.status === 'Bassa') statusElement.classList.add('status-low');
                    else if(data.status === 'Critica') statusElement.classList.add('status-critical');
                    else statusElement.classList.add('status-empty');
                })
                .catch(error => console.error('Errore:', error));
        }
        updateBattery();
        setInterval(updateBattery, 2000);
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleData() {
  float voltage = readBatteryVoltage();
  float percentage = getBatteryPercentage(voltage);
  String status = getBatteryStatus(voltage);
  
  // Aggiungi anche valore grezzo per debug
  float raw = readBatteryVoltageRaw();
  
  String json = "{";
  json += "\"voltage\":" + String(voltage, 2) + ",";
  json += "\"raw\":" + String(raw, 2) + ",";
  json += "\"percentage\":" + String(percentage, 1) + ",";
  json += "\"status\":\"" + status + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleCalibration() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang='it'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Calibrazione 3 Punti</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            padding: 40px;
            max-width: 600px;
            margin: 0 auto;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
        }
        h1 { color: #333; text-align: center; margin-bottom: 20px; font-size: 24px; }
        .help {
            background: #fff3cd;
            border: 1px solid #ffc107;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
            font-size: 14px;
        }
        .help strong { color: #856404; }
        .current-reading {
            background: #e3f2fd;
            padding: 20px;
            border-radius: 8px;
            margin-bottom: 20px;
            text-align: center;
        }
        .current-reading .voltage {
            font-size: 36px;
            font-weight: bold;
            color: #667eea;
        }
        .current-reading .raw {
            font-size: 14px;
            color: #999;
            margin-top: 5px;
        }
        .cal-point {
            background: #f8f9fa;
            padding: 20px;
            border-radius: 8px;
            margin-bottom: 15px;
        }
        .cal-point h3 {
            color: #667eea;
            margin-bottom: 15px;
            font-size: 16px;
        }
        .form-row {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
            margin-bottom: 10px;
        }
        .form-group { margin-bottom: 15px; }
        label {
            display: block;
            color: #666;
            font-size: 13px;
            font-weight: 500;
            margin-bottom: 5px;
        }
        input[type='number'] {
            width: 100%;
            padding: 10px;
            border: 2px solid #e0e0e0;
            border-radius: 6px;
            font-size: 15px;
        }
        input[type='number']:focus {
            outline: none;
            border-color: #667eea;
        }
        .checkbox-group {
            margin: 20px 0;
            padding: 15px;
            background: #f8f9fa;
            border-radius: 8px;
        }
        .checkbox-group label {
            display: flex;
            align-items: center;
            cursor: pointer;
        }
        .checkbox-group input[type='checkbox'] {
            width: 20px;
            height: 20px;
            margin-right: 10px;
        }
        .btn {
            width: 100%;
            padding: 14px;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: bold;
            cursor: pointer;
            margin-bottom: 10px;
        }
        .btn-primary { background: #667eea; color: white; }
        .btn-primary:hover { background: #5568d3; }
        .btn-secondary { background: #e0e0e0; color: #333; }
        .btn-secondary:hover { background: #d0d0d0; }
        .alert {
            padding: 12px;
            border-radius: 8px;
            margin-bottom: 20px;
            display: none;
        }
        .alert-success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
    </style>
</head>
<body>
    <div class='container'>
        <h1>🎯 Calibrazione a 3 Punti</h1>
        
        <div class='help'>
            <strong>📖 Come calibrare:</strong><br>
            1. Collega un multimetro preciso in parallelo alla batteria<br>
            2. Porta la batteria a 3 diverse tensioni (bassa, media, alta)<br>
            3. Per ogni tensione, leggi il valore sul multimetro e inseriscilo qui<br>
            4. Abilita la calibrazione e salva
        </div>
        
        <div class='current-reading'>
            <div style='font-size: 14px; color: #666; margin-bottom: 5px;'>Lettura Corrente (calibrata):</div>
            <div class='voltage' id='currentVoltage'>--.- V</div>
            <div class='raw'>Grezzo: <span id='rawVoltage'>--.-</span> V</div>
        </div>
        
        <div id='alert' class='alert'></div>
        
        <form id='calibrationForm'>
            <div class='cal-point'>
                <h3>🔵 Punto 1 - Basso (~11V)</h3>
                <div class='form-row'>
                    <div class='form-group'>
                        <label>Letto dal dispositivo:</label>
                        <input type='number' id='low_m' step='0.01' value=')rawliteral" + String(cal_low.measured, 2) + R"rawliteral(' required>
                    </div>
                    <div class='form-group'>
                        <label>Valore reale multimetro:</label>
                        <input type='number' id='low_a' step='0.01' value=')rawliteral" + String(cal_low.actual, 2) + R"rawliteral(' required>
                    </div>
                </div>
            </div>
            
            <div class='cal-point'>
                <h3>🟡 Punto 2 - Medio (~12V)</h3>
                <div class='form-row'>
                    <div class='form-group'>
                        <label>Letto dal dispositivo:</label>
                        <input type='number' id='mid_m' step='0.01' value=')rawliteral" + String(cal_mid.measured, 2) + R"rawliteral(' required>
                    </div>
                    <div class='form-group'>
                        <label>Valore reale multimetro:</label>
                        <input type='number' id='mid_a' step='0.01' value=')rawliteral" + String(cal_mid.actual, 2) + R"rawliteral(' required>
                    </div>
                </div>
            </div>
            
            <div class='cal-point'>
                <h3>🔴 Punto 3 - Alto (~12.5V)</h3>
                <div class='form-row'>
                    <div class='form-group'>
                        <label>Letto dal dispositivo:</label>
                        <input type='number' id='high_m' step='0.01' value=')rawliteral" + String(cal_high.measured, 2) + R"rawliteral(' required>
                    </div>
                    <div class='form-group'>
                        <label>Valore reale multimetro:</label>
                        <input type='number' id='high_a' step='0.01' value=')rawliteral" + String(cal_high.actual, 2) + R"rawliteral(' required>
                    </div>
                </div>
            </div>
            
            <div class='checkbox-group'>
                <label>
                    <input type='checkbox' id='enabled' )rawliteral" + String(calibration_enabled ? "checked" : "") + R"rawliteral(>
                    <span>✅ Abilita calibrazione a 3 punti</span>
                </label>
            </div>
            
            <button type='submit' class='btn btn-primary'>💾 Salva Calibrazione</button>
            <a href='/' class='btn btn-secondary' style='display:block; text-decoration:none; text-align:center;'>🔙 Torna al Monitor</a>
        </form>
    </div>
    <script>
        function updateVoltage() {
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('currentVoltage').textContent = data.voltage.toFixed(2) + ' V';
                    document.getElementById('rawVoltage').textContent = data.raw.toFixed(2);
                })
                .catch(error => console.error('Errore:', error));
        }
        
        updateVoltage();
        setInterval(updateVoltage, 2000);
        
        document.getElementById('calibrationForm').addEventListener('submit', function(e) {
            e.preventDefault();
            
            const data = {
                low_m: parseFloat(document.getElementById('low_m').value),
                low_a: parseFloat(document.getElementById('low_a').value),
                mid_m: parseFloat(document.getElementById('mid_m').value),
                mid_a: parseFloat(document.getElementById('mid_a').value),
                high_m: parseFloat(document.getElementById('high_m').value),
                high_a: parseFloat(document.getElementById('high_a').value),
                enabled: document.getElementById('enabled').checked ? '1' : '0'
            };
            
            const params = new URLSearchParams(data).toString();
            
            fetch('/save_calibration', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: params
            })
            .then(response => response.json())
            .then(data => {
                const alert = document.getElementById('alert');
                if(data.success) {
                    alert.className = 'alert alert-success';
                    alert.style.display = 'block';
                    alert.textContent = '✅ Calibrazione salvata! Tornando al monitor...';
                    setTimeout(() => {
                        window.location.href = '/';
                    }, 2000);
                }
            });
        });
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleSaveCalibration() {
  if (server.hasArg("low_m") && server.hasArg("low_a") && 
      server.hasArg("mid_m") && server.hasArg("mid_a") &&
      server.hasArg("high_m") && server.hasArg("high_a") &&
      server.hasArg("enabled")) {
    
    float low_m = server.arg("low_m").toFloat();
    float low_a = server.arg("low_a").toFloat();
    float mid_m = server.arg("mid_m").toFloat();
    float mid_a = server.arg("mid_a").toFloat();
    float high_m = server.arg("high_m").toFloat();
    float high_a = server.arg("high_a").toFloat();
    bool enabled = server.arg("enabled") == "1";
    
    saveCalibration(low_m, low_a, mid_m, mid_a, high_m, high_a, enabled);
    
    String json = "{\"success\":true}";
    server.send(200, "application/json", json);
  } else {
    String json = "{\"success\":false}";
    server.send(400, "application/json", json);
  }
}

void handleSettings() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang='it'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Parametri Batteria</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            padding: 40px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 500px;
            width: 100%;
        }
        h1 { color: #333; text-align: center; margin-bottom: 20px; font-size: 24px; }
        .info-box {
            background: #f8f9fa;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
            font-size: 14px;
        }
        .form-group { margin-bottom: 20px; }
        label {
            display: block;
            color: #666;
            font-size: 14px;
            font-weight: 500;
            margin-bottom: 8px;
        }
        input[type='number'] {
            width: 100%;
            padding: 12px;
            border: 2px solid #e0e0e0;
            border-radius: 8px;
            font-size: 16px;
        }
        input[type='number']:focus {
            outline: none;
            border-color: #667eea;
        }
        .help-text {
            font-size: 12px;
            color: #999;
            margin-top: 5px;
        }
        .btn {
            width: 100%;
            padding: 14px;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: bold;
            cursor: pointer;
            margin-bottom: 10px;
        }
        .btn-primary { background: #667eea; color: white; }
        .btn-primary:hover { background: #5568d3; }
        .btn-secondary { background: #e0e0e0; color: #333; }
        .btn-secondary:hover { background: #d0d0d0; }
        .alert {
            padding: 12px;
            border-radius: 8px;
            margin-bottom: 20px;
            display: none;
        }
        .alert-success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
    </style>
</head>
<body>
    <div class='container'>
        <h1>⚙️ Parametri Batteria</h1>
        
        <div class='info-box'>
            <strong>Valori Attuali:</strong><br>
            Min: )rawliteral" + String(BATTERY_MIN, 2) + R"rawliteral( V | Max: )rawliteral" + String(BATTERY_MAX, 2) + R"rawliteral( V
        </div>
        
        <div id='alert' class='alert'></div>
        
        <form id='settingsForm'>
            <div class='form-group'>
                <label>Tensione Minima (0%):</label>
                <input type='number' id='min' step='0.1' min='9' max='12' value=')rawliteral" + String(BATTERY_MIN, 1) + R"rawliteral(' required>
                <div class='help-text'>Batteria considerata scarica</div>
            </div>
            
            <div class='form-group'>
                <label>Tensione Massima (100%):</label>
                <input type='number' id='max' step='0.1' min='12' max='15' value=')rawliteral" + String(BATTERY_MAX, 1) + R"rawliteral(' required>
                <div class='help-text'>Batteria completamente carica</div>
            </div>
            
            <button type='submit' class='btn btn-primary'>💾 Salva Parametri</button>
            <a href='/' class='btn btn-secondary' style='display:block; text-decoration:none; text-align:center;'>🔙 Annulla</a>
        </form>
    </div>
    <script>
        document.getElementById('settingsForm').addEventListener('submit', function(e) {
            e.preventDefault();
            
            const min = parseFloat(document.getElementById('min').value);
            const max = parseFloat(document.getElementById('max').value);
            
            if(min >= max) {
                alert('❌ Min deve essere minore di Max!');
                return;
            }
            
            fetch('/save_settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'min=' + min + '&max=' + max
            })
            .then(response => response.json())
            .then(data => {
                const alert = document.getElementById('alert');
                if(data.success) {
                    alert.className = 'alert alert-success';
                    alert.style.display = 'block';
                    alert.textContent = '✅ Parametri salvati!';
                    setTimeout(() => {
                        window.location.href = '/';
                    }, 1500);
                }
            });
        });
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleSaveSettings() {
  if (server.hasArg("min") && server.hasArg("max")) {
    float min_v = server.arg("min").toFloat();
    float max_v = server.arg("max").toFloat();
    
    if(min_v > 0 && max_v > min_v) {
      saveBatteryParams(min_v, max_v);
      String json = "{\"success\":true}";
      server.send(200, "application/json", json);
    }
  }
}

void handleConfig() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang='it'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Configurazione IP</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            background: white;
            border-radius: 20px;
            padding: 40px;
            box-shadow: 0 20px 60px rgba(0,0,0,0.3);
            max-width: 500px;
            width: 100%;
        }
        h1 { color: #333; text-align: center; margin-bottom: 30px; font-size: 24px; }
        .current-config {
            background: #f8f9fa;
            padding: 15px;
            border-radius: 8px;
            margin-bottom: 20px;
            font-size: 14px;
        }
        .current-config strong { color: #667eea; }
        .form-group { margin-bottom: 20px; }
        label {
            display: block;
            color: #666;
            font-size: 14px;
            font-weight: 500;
            margin-bottom: 8px;
        }
        input[type='text'] {
            width: 100%;
            padding: 12px;
            border: 2px solid #e0e0e0;
            border-radius: 8px;
            font-size: 16px;
        }
        input[type='text']:focus {
            outline: none;
            border-color: #667eea;
        }
        .help-text {
            font-size: 12px;
            color: #999;
            margin-top: 5px;
        }
        .btn {
            width: 100%;
            padding: 14px;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: bold;
            cursor: pointer;
            margin-bottom: 10px;
        }
        .btn-primary { background: #667eea; color: white; }
        .btn-primary:hover { background: #5568d3; }
        .btn-secondary { background: #e0e0e0; color: #333; }
        .btn-secondary:hover { background: #d0d0d0; }
        .alert {
            padding: 12px;
            border-radius: 8px;
            margin-bottom: 20px;
            display: none;
        }
        .alert-success {
            background: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
    </style>
</head>
<body>
    <div class='container'>
        <h1>🌐 Configurazione IP Statico</h1>
        
        <div class='current-config'>
            <strong>Configurazione Attuale:</strong><br>
            IP: )rawliteral" + ETH.localIP().toString() + R"rawliteral(<br>
            Gateway: )rawliteral" + ETH.gatewayIP().toString() + R"rawliteral(<br>
            Subnet: )rawliteral" + ETH.subnetMask().toString() + R"rawliteral(
        </div>
        
        <div id='alert' class='alert'></div>
        
        <form id='networkForm'>
            <div class='form-group'>
                <label>Indirizzo IP:</label>
                <input type='text' id='ip' name='ip' placeholder='192.168.1.100' required>
                <div class='help-text'>Es: 192.168.1.100</div>
            </div>
            
            <div class='form-group'>
                <label>Gateway:</label>
                <input type='text' id='gateway' name='gateway' placeholder='192.168.1.1' required>
                <div class='help-text'>IP del tuo router</div>
            </div>
            
            <div class='form-group'>
                <label>Subnet Mask:</label>
                <input type='text' id='subnet' name='subnet' placeholder='255.255.255.0' required>
                <div class='help-text'>Solitamente 255.255.255.0</div>
            </div>
            
            <button type='submit' class='btn btn-primary'>💾 Salva e Riavvia</button>
            <a href='/' class='btn btn-secondary' style='display:block; text-decoration:none; text-align:center;'>🔙 Annulla</a>
        </form>
    </div>
    
    <script>
        document.getElementById('networkForm').addEventListener('submit', function(e) {
            e.preventDefault();
            
            const ip = document.getElementById('ip').value;
            const gateway = document.getElementById('gateway').value;
            const subnet = document.getElementById('subnet').value;
            
            // Validazione base
            if(!ip || !gateway || !subnet) {
                alert('❌ Compila tutti i campi!');
                return;
            }
            
            fetch('/save_config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'ip=' + ip + '&gateway=' + gateway + '&subnet=' + subnet
            })
            .then(response => response.json())
            .then(data => {
                const alert = document.getElementById('alert');
                if(data.success) {
                    alert.className = 'alert alert-success';
                    alert.style.display = 'block';
                    alert.textContent = '✅ IP salvato! Riavvio in corso... Il dispositivo sarà disponibile su: ' + ip;
                    setTimeout(() => {
                        window.location.href = 'http://' + ip;
                    }, 5000);
                }
            })
            .catch(error => {
                alert('❌ Errore durante il salvataggio');
            });
        });
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleSaveConfig() {
  if (server.hasArg("ip") && server.hasArg("gateway") && server.hasArg("subnet")) {
    IPAddress new_ip, new_gw, new_sn;
    
    if (new_ip.fromString(server.arg("ip")) && 
        new_gw.fromString(server.arg("gateway")) && 
        new_sn.fromString(server.arg("subnet"))) {
      
      saveNetworkConfig(new_ip, new_gw, new_sn);
      
      String json = "{\"success\":true}";
      server.send(200, "application/json", json);
      
      delay(1000);
      ESP.restart();
    } else {
      String json = "{\"success\":false,\"message\":\"IP non validi\"}";
      server.send(400, "application/json", json);
    }
  } else {
    String json = "{\"success\":false,\"message\":\"Parametri mancanti\"}";
    server.send(400, "application/json", json);
  }
}

void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("⚡ ETH Started");
      eth_started = true;
      ETH.setHostname("battery-monitor");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("🔌 Link UP");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.printf("✅ IP: %s\n", ETH.localIP().toString().c_str());
      eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      eth_connected = false;
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════════════╗");
  Serial.println("║  Battery Monitor - 3-Point Calibration        ║");
  Serial.println("╚════════════════════════════════════════════════╝\n");
  
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  
  for(int i = 0; i < MOVING_AVG_SIZE; i++) {
    voltage_history[i] = 0;
  }
  
  loadNetworkConfig();
  loadBatteryParams();
  loadCalibration();
  
  Serial.printf("\n📍 IP: %s\n", local_ip.toString().c_str());
  
  Network.onEvent(onEvent);
  
  Serial.println("🚀 Avvio Ethernet...");
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE);
  delay(1000);
  ETH.config(local_ip, gateway, subnet, dns1, dns2);
  
  Serial.print("⏳ Attesa");
  for(int i = 0; i < 20 && !ETH.linkUp(); i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  if(ETH.linkUp()) {
    delay(3000);
    for(int i = 0; i < 15; i++) {
      IPAddress current = ETH.localIP();
      if(current != IPAddress(0,0,0,0) && current == local_ip) {
        eth_connected = true;
        break;
      }
      delay(1000);
    }
  }
  
  if(eth_connected) {
    Serial.println("\n✅ Sistema pronto!");
    Serial.printf("🌐 http://%s\n\n", ETH.localIP().toString().c_str());
    
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/calibration", handleCalibration);
    server.on("/save_calibration", HTTP_POST, handleSaveCalibration);
    server.on("/settings", handleSettings);
    server.on("/save_settings", HTTP_POST, handleSaveSettings);
    server.on("/config", handleConfig);
    server.on("/save_config", HTTP_POST, handleSaveConfig);
    server.begin();
  } else {
    Serial.println("\n❌ Connessione fallita\n");
  }
}

void loop() {
  server.handleClient();
  
  static unsigned long lastPrint = 0;
  if(millis() - lastPrint > 5000) {
    lastPrint = millis();
    
    if(eth_connected) {
      float voltage = readBatteryVoltage();
      float percentage = getBatteryPercentage(voltage);
      
      Serial.printf("🔋 %.2fV (%.0f%%) | Cal:%s | %s\n", 
        voltage, percentage, 
        calibration_enabled ? "ON" : "OFF",
        ETH.localIP().toString().c_str());
    }
  }
}
