#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <driver/i2s.h>

// --- 設定區 ---
const char* ssid = "louisguan";
const char* password = "0989839679";

#define I2S_WS 4
#define I2S_SD 6
#define I2S_SCK 5
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define DMA_BUF_LEN 1024

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// --- 前端網頁 HTML ---
// 使用 PROGMEM 確保 HTML 存放在 Flash 中，不佔用 RAM
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>ESP32-S3 音訊分析儀</title>
    <style>
        body { background: #121212; color: #eee; font-family: sans-serif; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; }
        .card { background: #1e1e1e; padding: 20px; border-radius: 12px; width: 100%; max-width: 800px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }
        h2 { color: #00e676; text-align: center; }
        canvas { width: 100%; height: 160px; background: #000; border-radius: 4px; margin: 10px 0; border: 1px solid #333; }
        .controls { display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 10px; margin: 20px 0; }
        button { padding: 12px; border: none; border-radius: 6px; cursor: pointer; background: #333; color: white; font-weight: bold; transition: 0.2s; }
        button.active { background: #00e676; color: #000; }
        #vu-meter { width: 100%; height: 12px; background: #333; border-radius: 6px; overflow: hidden; margin-bottom: 5px; }
        #vu-bar { height: 100%; width: 0%; background: linear-gradient(90deg, #00e676, #ffeb3b, #ff5252); transition: width 0.05s; }
    </style>
</head>
<body>
    <div class="card">
        <h2>ESP32-S3 x INMP441 分析儀</h2>
        <div id="vu-meter"><div id="vu-bar"></div></div>
        <canvas id="waveformCanvas"></canvas>
        <canvas id="spectrumCanvas"></canvas>
        <div class="controls">
            <button id="startBtn" onclick="initAudio()" style="background:#2196F3">1. 啟動音訊連線</button>
            <button onclick="setFilter('none')" id="f-none" class="active">原始聲音</button>
            <button onclick="setFilter('lowpass')" id="f-low">人聲增強(LPF)</button>
            <button onclick="setFilter('highpass')" id="f-high">過濾低噪(HPF)</button>
            <button onclick="toggleAGC()" id="agc-btn">AGC: 關閉</button>
            <button onclick="toggleRecord()" id="rec-btn" style="background:#f44336">開始錄音</button>
        </div>
        <div id="dl-box" style="text-align:center"></div>
    </div>
    <script>
        let audioCtx, filter, compressor, analyser, dataArray, dest, recorder, chunks = [];
        let isAGC = false, isRecording = false, socket;

        function initAudio() {
            if (audioCtx) return;
            audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
            filter = audioCtx.createBiquadFilter(); filter.type = "allpass";
            compressor = audioCtx.createDynamicsCompressor(); // AGC
            analyser = audioCtx.createAnalyser(); analyser.fftSize = 1024;
            dataArray = new Uint8Array(analyser.frequencyBinCount);
            dest = audioCtx.createMediaStreamDestination();

            socket = new WebSocket(`ws://${window.location.hostname}/ws`);
            socket.binaryType = 'arraybuffer';
            socket.onmessage = (e) => {
                let input = new Int16Array(e.data);
                let floatData = new Float32Array(input.length);
                for (let i = 0; i < input.length; i++) floatData[i] = input[i] / 32768.0;
                let buf = audioCtx.createBuffer(1, floatData.length, 16000);
                buf.getChannelData(0).set(floatData);
                let src = audioCtx.createBufferSource(); src.buffer = buf;
                src.connect(filter);
                let node = filter; if(isAGC) { filter.connect(compressor); node = compressor; }
                node.connect(analyser); analyser.connect(audioCtx.destination); analyser.connect(dest);
                src.start();
            };
            document.getElementById('startBtn').innerText = "連線成功";
            draw();
        }

        function setFilter(t) {
            document.querySelectorAll('.controls button').forEach(b => b.classList.remove('active'));
            if(t==='none'){ filter.type="allpass"; document.getElementById('f-none').classList.add('active'); }
            else { filter.type=t; filter.frequency.value=(t==='lowpass')?2800:350; document.getElementById('f-'+(t==='lowpass'?'low':'high')).classList.add('active'); }
        }

        function toggleAGC() { isAGC=!isAGC; document.getElementById('agc-btn').innerText="AGC: "+(isAGC?"開啟":"關閉"); document.getElementById('agc-btn').classList.toggle('active'); }

        function toggleRecord() {
            if(!isRecording) {
                chunks = []; recorder = new MediaRecorder(dest.stream);
                recorder.ondataavailable = e => chunks.push(e.data);
                recorder.onstop = () => {
                    let blob = new Blob(chunks, {type:'audio/wav'});
                    let a = document.createElement('a'); a.href=URL.createObjectURL(blob); a.download='record.wav'; a.innerText='下載錄音檔';
                    document.getElementById('dl-box').innerHTML=''; document.getElementById('dl-box').appendChild(a);
                };
                recorder.start(); isRecording=true; document.getElementById('rec-btn').innerText="停止錄音";
            } else { recorder.stop(); isRecording=false; document.getElementById('rec-btn').innerText="開始錄音"; }
        }

        function draw() {
            requestAnimationFrame(draw);
            analyser.getByteFrequencyData(dataArray);
            let sCtx = document.getElementById('spectrumCanvas').getContext('2d');
            sCtx.fillStyle='#000'; sCtx.fillRect(0,0,800,160);
            for(let i=0; i<dataArray.length; i++) {
                sCtx.fillStyle=`hsl(${i}, 100%, 50%)`;
                sCtx.fillRect(i*3, 160-dataArray[i]/1.5, 2, dataArray[i]/1.5);
            }
            analyser.getByteTimeDomainData(dataArray);
            let wCtx = document.getElementById('waveformCanvas').getContext('2d');
            wCtx.fillStyle='#000'; wCtx.fillRect(0,0,800,160);
            wCtx.strokeStyle='#00e676'; wCtx.beginPath();
            let rms = 0;
            for(let i=0; i<dataArray.length; i++) {
                let y = (dataArray[i]/128.0)*80;
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

// --- ESP32 後端功能 ---
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
    setupI2S();
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println(WiFi.localIP());

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
