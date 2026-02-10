#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <driver/i2s.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// [OLED 設定] 128x32 SSD1306
U8G2_SSD1306_128X32_UNIVISION_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// === 硬體腳位定義 ===
#define I2C_SDA_PIN 41 // OLED SDA
#define I2C_SCL_PIN 42 // OLED SCL
#define I2S_WS 4       // INMP441 WS
#define I2S_SD 6       // INMP441 SD
#define I2S_SCK 5      // INMP441 SCK
#define I2S_PORT I2S_NUM_0

// === WiFi 設定 ===
const char* ssid = "louisguan";
const char* password = "0989839679";

// === 全域變數 ===
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
#define SAMPLE_RATE 16000
#define DMA_BUF_LEN 1024

// --- 前端網頁 HTML ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8"><title>ESP32-S3 Voice Tool</title>
    <style>
        body { background: #121212; color: #eee; font-family: sans-serif; text-align: center; margin: 0; padding: 20px; }
        .card { background: #1e1e1e; padding: 20px; border-radius: 12px; max-width: 600px; margin: auto; }
        canvas { width: 100%; height: 120px; background: #000; margin: 10px 0; border-radius: 4px; }
        .controls { display: flex; flex-wrap: wrap; justify-content: center; gap: 8px; }
        button { padding: 8px 12px; cursor: pointer; border-radius: 4px; border: none; background: #333; color: white; }
        button.active { background: #00e676; color: black; }
        #vu { width: 100%; height: 10px; background: #333; border-radius: 5px; overflow: hidden; }
        #vu-bar { height: 100%; width: 0%; background: lime; transition: 0.1s; }
    </style>
</head>
<body>
    <div class="card">
        <h2>人聲分析面板</h2>
        <div id="vu"><div id="vu-bar"></div></div>
        <canvas id="waveform"></canvas>
        <canvas id="spectrum"></canvas>
        <div class="controls">
            <button id="startBtn" onclick="initAudio()" style="background:#2196F3">啟動連線</button>
            <button onclick="setFilter('none')" id="f-none" class="active">原始</button>
            <button onclick="setFilter('lowpass')">人聲增強</button>
            <button onclick="setFilter('highpass')">去低頻噪</button>
            <button onclick="toggleAGC()" id="agc-btn">AGC: 關閉</button>
            <button onclick="toggleRecord()" id="rec-btn" style="background:#f44336">開始錄音</button>
        </div>
        <div id="dl" style="margin-top:10px"></div>
    </div>
    <script>
        let audioCtx, filter, compressor, analyser, dataArray, dest, recorder, chunks=[];
        let isAGC=false, isRec=false, socket;
        function initAudio() {
            audioCtx = new (window.AudioContext || window.webkitAudioContext)({sampleRate: 16000});
            filter = audioCtx.createBiquadFilter(); filter.type="allpass";
            compressor = audioCtx.createDynamicsCompressor(); 
            analyser = audioCtx.createAnalyser(); analyser.fftSize=1024;
            dataArray = new Uint8Array(analyser.frequencyBinCount);
            dest = audioCtx.createMediaStreamDestination();
            socket = new WebSocket(`ws://${window.location.hostname}/ws`);
            socket.binaryType = 'arraybuffer';
            socket.onmessage = (e) => {
                let input = new Int16Array(e.data);
                let floatData = new Float32Array(input.length);
                for(let i=0; i<input.length; i++) floatData[i] = input[i]/32768.0;
                let buf = audioCtx.createBuffer(1, floatData.length, 16000);
                buf.getChannelData(0).set(floatData);
                let src = audioCtx.createBufferSource(); src.buffer = buf;
                src.connect(filter);
                let node = filter; if(isAGC) { filter.connect(compressor); node = compressor; }
                node.connect(analyser); analyser.connect(audioCtx.destination); analyser.connect(dest);
                src.start();
            };
            document.getElementById('startBtn').style.display='none';
            draw();
        }
        function setFilter(t) {
            filter.type = (t==='none') ? "allpass" : t;
            if(t!=='none') filter.frequency.value = (t==='lowpass') ? 2500 : 350;
        }
        function toggleAGC() { isAGC=!isAGC; document.getElementById('agc-btn').innerText="AGC: "+(isAGC?"開啟":"關閉"); }
        function toggleRecord() {
            if(!isRec) {
                chunks=[]; recorder=new MediaRecorder(dest.stream);
                recorder.ondataavailable=e=>chunks.push(e.data);
                recorder.onstop=()=>{
                    let b=new Blob(chunks,{type:'audio/wav'});
                    let a=document.createElement('a'); a.href=URL.createObjectURL(b); a.download='rec.wav'; a.innerText='下載錄音檔';
                    document.getElementById('dl').innerHTML=''; document.getElementById('dl').appendChild(a);
                };
                recorder.start(); isRec=true; document.getElementById('rec-btn').innerText="停止錄音";
            } else { recorder.stop(); isRec=false; document.getElementById('rec-btn').innerText="開始錄音"; }
        }
        function draw() {
            requestAnimationFrame(draw);
            analyser.getByteFrequencyData(dataArray);
            let sCtx=document.getElementById('spectrum').getContext('2d');
            sCtx.fillStyle='#000'; sCtx.fillRect(0,0,800,150);
            for(let i=0; i<dataArray.length; i++) {
                sCtx.fillStyle='#00e676'; sCtx.fillRect(i*3, 150-dataArray[i]/1.5, 2, dataArray[i]/1.5);
            }
            analyser.getByteTimeDomainData(dataArray);
            let wCtx=document.getElementById('waveform').getContext('2d');
            wCtx.fillStyle='#000'; wCtx.fillRect(0,0,800,150);
            wCtx.strokeStyle='#2196F3'; wCtx.beginPath();
            let rms=0;
            for(let i=0; i<dataArray.length; i++) {
                let y=(dataArray[i]/128.0)*75;
                if(i===0) wCtx.moveTo(0,y); else wCtx.lineTo(i*(800/dataArray.length),y);
                rms += Math.abs(dataArray[i]-128);
            }
            wCtx.stroke();
            document.getElementById('vu-bar').style.width = Math.min(100, rms*0.8) + "%";
        }
    </script>
</body>
</html>
)rawliteral";

// --- 初始化 I2S ---
void setupI2S() {
    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false
    };
    i2s_pin_config_t pins = { .bck_io_num = I2S_SCK, .ws_io_num = I2S_WS, .data_out_num = -1, .data_in_num = I2S_SD };
    i2s_driver_install(I2S_PORT, &config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pins);
}

void setup() {
    Serial.begin(115200);
    
    // 初始化 I2C 與 OLED
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tf);
    
    setupI2S();

    // WiFi 連線顯示
    u8g2.firstPage();
    do { 
        u8g2.drawStr(0, 15, "WiFi: louisguan"); 
        u8g2.drawStr(0, 30, "Connecting..."); 
    } while ( u8g2.nextPage() );
    
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    
    // 連線成功顯示 IP 地址
    String ip = WiFi.localIP().toString();
    u8g2.firstPage();
    do { 
        u8g2.drawStr(0, 12, "WiFi Connected!"); 
        u8g2.drawStr(0, 28, ("IP: " + ip).c_str()); 
    } while ( u8g2.nextPage() );

    server.addHandler(&ws);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", index_html); });
    server.begin();
}

void loop() {
    int16_t buf[DMA_BUF_LEN];
    size_t read;
    if (i2s_read(I2S_PORT, &buf, sizeof(buf), &read, portMAX_DELAY) == ESP_OK && read > 0) {
        if (ws.count() > 0) ws.binaryAll((uint8_t*)buf, read);
    }
}
