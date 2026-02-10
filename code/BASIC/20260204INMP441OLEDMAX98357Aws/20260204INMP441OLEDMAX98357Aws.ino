#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <driver/i2s.h>
#include <LittleFS.h>

#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// --- 硬體腳位定義 (保留您的設定) ---
#define I2C_SDA_PIN 41
#define I2C_SCL_PIN 42

// INMP441 麥克風 (I2S_NUM_0)
#define I2S_WS 4
#define I2S_SD 6
#define I2S_SCK 5

// MAX98357A 喇叭 (I2S_NUM_1)
#define I2S_SPK_LRC 16
#define I2S_SPK_DIN 7
#define I2S_SPK_BCLK 15

// --- 參數設定 ---
const char* ssid = "louisguan";
const char* password = "0989839679";
#define SAMPLE_RATE 16000
#define DMA_BUF_LEN 1024

// --- 音量放大倍率 (解決音量過小問題) ---
#define MIC_GAIN_FACTOR 4

// --- 全域物件 ---
U8G2_SSD1306_128X32_UNIVISION_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
File recordFile;
bool isRecordingToFlash = false;
volatile bool isPlaying = false;

// --- 前端網頁 HTML ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8"><title>ESP32-S3 Pro Audio Hub</title>
    <style>
        body { background: #121212; color: #eee; font-family: sans-serif; text-align: center; padding: 15px; margin: 0; }
        .card { background: #1e1e1e; padding: 20px; border-radius: 15px; max-width: 800px; margin: auto; border: 1px solid #333; }
        h2 { color: #00e676; margin-top: 0; }
        canvas { width: 100%; height: 130px; background: #000; border-radius: 8px; margin: 5px 0; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 10px; margin-top: 15px; }
        button { padding: 12px; cursor: pointer; border-radius: 8px; border: none; background: #333; color: white; font-weight: bold; transition: 0.2s; }
        button.active { background: #2196F3; }
        button.rec-active { background: #f44336; animation: blink 1.5s infinite; }
        @keyframes blink { 0% { opacity: 1; } 50% { opacity: 0.6; } 100% { opacity: 1; } }
        #vu-container { width: 100%; height: 12px; background: #333; border-radius: 10px; overflow: hidden; margin: 10px 0; border: 1px solid #444; }
        #vu-bar { height: 100%; width: 0%; background: linear-gradient(90deg, #00e676, #ffeb3b, #ff5252); transition: width 0.05s; }
        .label { font-size: 0.75em; color: #888; text-align: left; margin-left: 5px; }
    </style>
</head>
<body>
    <div class="card">
        <h2>語音分析錄放工作站</h2>
        <div id="vu-container"><div id="vu-bar"></div></div>
        <div class="label">實時波形 / FFT 頻譜</div>
        <canvas id="waveform"></canvas>
        <canvas id="spectrum"></canvas>
        <button id="startBtn" onclick="initAudio()" style="background:#00e676; color:#000; width:100%; font-size:1.1em;">啟動音訊連線</button>
        <div class="grid">
            <button id="btn-agc" onclick="toggleAGC()">自動增益 (AGC)</button>
            <button id="btn-hp" onclick="toggleFilter('highpass')">100Hz 高通</button>
            <button id="btn-lp" onclick="toggleFilter('lowpass')">3kHz 低通</button>
            <button id="btn-web-rec" onclick="toggleWebRecord()">網頁錄音</button>
            <button id="recBtn" onclick="toggleFlashRecord()" style="border: 1px solid #f44336;">Flash 錄音</button>
            <button id="playBtn" onclick="playFromFlash()" style="background:#ff9800">裝置播放</button>
        </div>
        <div id="downloadArea" style="margin-top:20px"></div>
    </div>
    <script>
        let audioCtx, analyser, dataArray, waveArray, socket, hpFilter, lpFilter, compressor, webRecorder, chunks = [];
        let isAGC = false, isHP = false, isLP = false, isWebRec = false;

        async function initAudio() {
            audioCtx = new (window.AudioContext || window.webkitAudioContext)({sampleRate: 16000});
            analyser = audioCtx.createAnalyser(); analyser.fftSize = 1024;
            dataArray = new Uint8Array(analyser.frequencyBinCount);
            waveArray = new Uint8Array(analyser.fftSize);

            hpFilter = audioCtx.createBiquadFilter(); hpFilter.type = "highpass"; hpFilter.frequency.value = 100;
            lpFilter = audioCtx.createBiquadFilter(); lpFilter.type = "lowpass"; lpFilter.frequency.value = 3000;
            compressor = audioCtx.createDynamicsCompressor(); compressor.threshold.value = -35;

            socket = new WebSocket(`ws://${window.location.hostname}/ws`);
            socket.binaryType = 'arraybuffer';
            let dest = audioCtx.createMediaStreamDestination();
            webRecorder = new MediaRecorder(dest.stream);
            webRecorder.ondataavailable = e => chunks.push(e.data);
            webRecorder.onstop = () => {
                let blob = new Blob(chunks, {type: 'audio/wav'});
                let a = document.createElement('a'); a.href = URL.createObjectURL(blob);
                a.download = 'web_rec.wav'; a.innerText = '⬇️ 下載網頁錄音檔'; a.style.color = '#00e676';
                document.getElementById('downloadArea').innerHTML = ''; document.getElementById('downloadArea').appendChild(a);
            };

            socket.onmessage = (e) => {
                let input = new Int16Array(e.data);
                let floatData = new Float32Array(input.length);
                for(let i=0; i<input.length; i++) floatData[i] = input[i]/32768.0;
                let buf = audioCtx.createBuffer(1, floatData.length, 16000);
                buf.getChannelData(0).set(floatData);
                let src = audioCtx.createBufferSource(); src.buffer = buf;
                let node = src;
                if(isHP) { node.connect(hpFilter); node = hpFilter; }
                if(isLP) { node.connect(lpFilter); node = lpFilter; }
                if(isAGC) { node.connect(compressor); node = compressor; }
                node.connect(analyser); analyser.connect(audioCtx.destination); analyser.connect(dest);
                src.start();
            };
            document.getElementById('startBtn').style.display='none';
            draw();
        }

        function toggleAGC() { isAGC = !isAGC; document.getElementById('btn-agc').classList.toggle('active'); }
        function toggleFilter(t) {
            if(t==='highpass') { isHP = !isHP; document.getElementById('btn-hp').classList.toggle('active'); }
            if(t==='lowpass') { isLP = !isLP; document.getElementById('btn-lp').classList.toggle('active'); }
        }
        function toggleWebRecord() {
            if(!isWebRec) { chunks = []; webRecorder.start(); document.getElementById('btn-web-rec').classList.add('rec-active'); }
            else { webRecorder.stop(); document.getElementById('btn-web-rec').classList.remove('rec-active'); }
            isWebRec = !isWebRec;
        }
        function toggleFlashRecord() {
            fetch('/toggleRecord').then(() => document.getElementById('recBtn').classList.toggle('rec-active'));
        }
        function playFromFlash() { fetch('/playRecord'); }

        function draw() {
            requestAnimationFrame(draw);
            analyser.getByteFrequencyData(dataArray);
            let sCtx = document.getElementById('spectrum').getContext('2d');
            sCtx.fillStyle='black'; sCtx.fillRect(0,0,800,150);
            for(let i=0; i<dataArray.length; i++) {
                sCtx.fillStyle = `hsl(${i}, 80%, 50%)`;
                sCtx.fillRect(i*3, 150 - dataArray[i]/1.5, 2, dataArray[i]/1.5);
            }
            analyser.getByteTimeDomainData(waveArray);
            let wCtx = document.getElementById('waveform').getContext('2d');
            wCtx.fillStyle='black'; wCtx.fillRect(0,0,800,150);
            wCtx.strokeStyle='#00e676'; wCtx.beginPath();
            let rms = 0;
            for(let i=0; i<waveArray.length; i++) {
                let y = (waveArray[i]/128.0)*75;
                if(i===0) wCtx.moveTo(0,y); else wCtx.lineTo(i*(800/waveArray.length),y);
                rms += Math.abs(waveArray[i]-128);
            }
            wCtx.stroke();
            document.getElementById('vu-bar').style.width = Math.min(100, rms*0.8) + "%";
        }
    </script>
</body>
</html>
)rawliteral";

// --- 播放任務 ---
void playTask(void *parameter) {
    isPlaying = true;
    File file = LittleFS.open("/voice.pcm", FILE_READ);
    if (file) {
        uint8_t* playBuf = (uint8_t*)ps_malloc(2048); 
        size_t bytesWritten;
        while (file.available() && isPlaying) {
            int l = file.read(playBuf, 2048);
            i2s_write(I2S_NUM_1, playBuf, l, &bytesWritten, portMAX_DELAY);
        }
        free(playBuf);
        file.close();
    }
    isPlaying = false;
    vTaskDelete(NULL);
}

void setupI2S() {
    i2s_config_t mic_cfg = { .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), .sample_rate = SAMPLE_RATE, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = I2S_COMM_FORMAT_STAND_I2S, .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8, .dma_buf_len = DMA_BUF_LEN, .use_apll = false };
    i2s_driver_install(I2S_NUM_0, &mic_cfg, 0, NULL);
    i2s_pin_config_t mic_pins = { .bck_io_num = I2S_SCK, .ws_io_num = I2S_WS, .data_out_num = -1, .data_in_num = I2S_SD };
    i2s_set_pin(I2S_NUM_0, &mic_pins);

    i2s_config_t spk_cfg = { .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX), .sample_rate = SAMPLE_RATE, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = I2S_COMM_FORMAT_STAND_I2S, .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8, .dma_buf_len = DMA_BUF_LEN, .use_apll = false };
    i2s_driver_install(I2S_NUM_1, &spk_cfg, 0, NULL);
    i2s_pin_config_t spk_pins = { .bck_io_num = I2S_SPK_BCLK, .ws_io_num = I2S_SPK_LRC, .data_out_num = I2S_SPK_DIN, .data_in_num = -1 };
    i2s_set_pin(I2S_NUM_1, &spk_pins);
}

void setup() {
    Serial.begin(115200);
    LittleFS.begin(true);
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    u8g2.begin();
    u8g2.setFont(u8g2_font_6x12_tf);
    setupI2S();

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(500);
    
    String ip = WiFi.localIP().toString();
    u8g2.firstPage();
    do { u8g2.drawStr(0, 15, "louisguan Link!"); u8g2.drawStr(0, 30, ip.c_str()); } while ( u8g2.nextPage() );

    // --- 修改後的錄音觸發路由 ---
    server.on("/toggleRecord", HTTP_GET, [](AsyncWebServerRequest *r){
        if(!isRecordingToFlash) {
            // 第一步：如果舊檔案存在，先清除它
            if (LittleFS.exists("/voice.pcm")) {
                LittleFS.remove("/voice.pcm");
                Serial.println("Old record cleared.");
            }
            // 第二步：開始新的錄製
            recordFile = LittleFS.open("/voice.pcm", FILE_WRITE);
            isRecordingToFlash = true;
            r->send(200);
        } else {
            isRecordingToFlash = false;
            recordFile.close();
            r->send(200);
        }
    });

    server.on("/playRecord", HTTP_GET, [](AsyncWebServerRequest *r){
        if(!isPlaying) { xTaskCreate(playTask, "playTask", 8192, NULL, 5, NULL); r->send(200); }
        else r->send(200);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", index_html); });
    server.addHandler(&ws);
    server.begin();
}

void loop() {
    int16_t buf[DMA_BUF_LEN];
    size_t read;
    if (i2s_read(I2S_NUM_0, &buf, sizeof(buf), &read, portMAX_DELAY) == ESP_OK && read > 0) {
        // 數位音量增強
        for (int i = 0; i < read / 2; i++) {
            int32_t amp = (int32_t)buf[i] * MIC_GAIN_FACTOR;
            buf[i] = (int16_t)constrain(amp, -32768, 32767);
        }
        if (!isPlaying && ws.count() > 0) ws.binaryAll((uint8_t*)buf, read);
        if (isRecordingToFlash && recordFile) recordFile.write((const uint8_t*)buf, read);
    }
}
