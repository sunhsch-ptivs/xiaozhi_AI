#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
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
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>ESP32-S3 音訊分析儀</title>
    <style>
        body { background: #121212; color: #eee; font-family: 'Segoe UI', sans-serif; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; }
        .card { background: #1e1e1e; padding: 20px; border-radius: 12px; box-shadow: 0 4px 20px rgba(0,0,0,0.5); width: 100%; max-width: 800px; }
        h2 { margin-top: 0; color: #00e676; text-align: center; }
        canvas { width: 100%; height: 150px; background: #000; border-radius: 6px; margin: 10px 0; border: 1px solid #333; }
        .controls { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; margin: 20px 0; }
        button { padding: 12px; border: none; border-radius: 6px; cursor: pointer; background: #333; color: white; font-weight: bold; transition: 0.2s; }
        button:hover { background: #444; }
        button.active { background: #00e676; color: #000; }
        #vu-meter { width: 100%; height: 12px; background: #333; border-radius: 6px; overflow: hidden; margin-bottom: 5px; }
        #vu-bar { height: 100%; width: 0%; background: linear-gradient(90deg, #00e676 70%, #ff5252 100%); transition: width 0.05s; }
        .status { font-size: 0.8em; color: #888; text-align: center; }
    </style>
</head>
<body>
    <div class="card">
        <h2>ESP32-S3 音訊監控</h2>
        <div class="status" id="ws-status">等待連線...</div>
        
        <div id="vu-meter"><div id="vu-bar"></div></div>
        <canvas id="waveformCanvas"></canvas>
        <canvas id="spectrumCanvas"></canvas>

        <div class="controls">
            <button id="startBtn" onclick="initAudio()" style="background:#2196F3">1. 啟動音訊</button>
            <button onclick="setFilter('none')" id="f-none" class="active">原始</button>
            <button onclick="setFilter('lowpass')" id="f-low">人聲增強</button>
            <button onclick="setFilter('highpass')" id="f-high">過濾低噪</button>
            <button onclick="toggleAGC()" id="agc-btn">AGC: 關閉</button>
            <button onclick="toggleRecord()" id="rec-btn" style="background:#f44336">開始錄音</button>
        </div>
        <div id="download-link" style="text-align:center"></div>
    </div>

    <script>
        let audioCtx, filter, compressor, analyser, dataArray;
        let isAGC = false, isRecording = false, mediaRecorder, dest;
        let socket;

        function initAudio() {
            if (audioCtx) return;
            audioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
            
            // 處理鏈設定
            filter = audioCtx.createBiquadFilter();
            filter.type = "allpass"; 

            compressor = audioCtx.createDynamicsCompressor(); // AGC 核心
            compressor.threshold.value = -40; // 觸發壓縮的閾值
            compressor.ratio.value = 12;      // 壓縮比

            analyser = audioCtx.createAnalyser();
            analyser.fftSize = 1024;
            dataArray = new Uint8Array(analyser.frequencyBinCount);

            // 錄音準備
            dest = audioCtx.createMediaStreamDestination();

            // WebSocket 連線
            const gateway = `ws://${window.location.hostname}/ws`;
            socket = new WebSocket(gateway);
            socket.binaryType = 'arraybuffer';

            socket.onopen = () => {
                document.getElementById('ws-status').innerText = "已連線至 ESP32";
                document.getElementById('startBtn').style.display = "none";
            };

            socket.onmessage = (event) => {
                let inputData = new Int16Array(event.data);
                let floatData = new Float32Array(inputData.length);
                for (let i = 0; i < inputData.length; i++) floatData[i] = inputData[i] / 32768.0;

                let audioBuffer = audioCtx.createBuffer(1, floatData.length, 16000);
                audioBuffer.getChannelData(0).set(floatData);
                
                let source = audioCtx.createBufferSource();
                source.buffer = audioBuffer;

                // 鏈結: Source -> Filter -> (AGC) -> Analyser -> Destination & Stream
                source.connect(filter);
                let lastNode = filter;
                if(isAGC) { filter.connect(compressor); lastNode = compressor; }
                
                lastNode.connect(analyser);
                analyser.connect(audioCtx.destination);
                analyser.connect(dest); // 同時輸送到錄音節點
                source.start();
            };

            draw();
        }

        function setFilter(type) {
            document.querySelectorAll('.controls button').forEach(b => b.classList.remove('active'));
            if(type === 'none') {
                filter.type = "allpass";
                document.getElementById('f-none').classList.add('active');
            } else {
                filter.type = type;
                filter.frequency.value = (type === 'lowpass') ? 2500 : 300;
                document.getElementById('f-' + (type==='lowpass'?'low':'high')).classList.add('active');
            }
        }

        function toggleAGC() {
            isAGC = !isAGC;
            document.getElementById('agc-btn').innerText = "AGC: " + (isAGC ? "開啟" : "關閉");
            document.getElementById('agc-btn').classList.toggle('active');
        }

        function toggleRecord() {
            if (!isRecording) {
                chunks = [];
                mediaRecorder = new MediaRecorder(dest.stream);
                mediaRecorder.ondataavailable = e => chunks.push(e.data);
                mediaRecorder.onstop = () => {
                    const blob = new Blob(chunks, { type: 'audio/wav' });
                    const url = URL.createObjectURL(blob);
                    const a = document.createElement('a');
                    a.href = url; a.download = 'recording.wav';
                    a.innerText = '點此下載最後錄音檔';
                    document.getElementById('download-link').innerHTML = '';
                    document.getElementById('download-link').appendChild(a);
                };
                mediaRecorder.start();
                document.getElementById('rec-btn').innerText = "停止錄音";
                isRecording = true;
            } else {
                mediaRecorder.stop();
                document.getElementById('rec-btn').innerText = "開始錄音";
                isRecording = false;
            }
        }

        function draw() {
            requestAnimationFrame(draw);
            
            // 繪製頻譜 (FFT)
            analyser.getByteFrequencyData(dataArray);
            let ctxS = document.getElementById('spectrumCanvas').getContext('2d');
            ctxS.fillStyle = '#000'; ctxS.fillRect(0,0,800,150);
            for(let i=0; i<dataArray.length; i++) {
                let barHeight = dataArray[i];
                ctxS.fillStyle = `rgb(0, ${barHeight+100}, 255)`;
                ctxS.fillRect(i * 3, 150 - barHeight/1.5, 2, barHeight/1.5);
            }

            // 繪製波形
            analyser.getByteTimeDomainData(dataArray);
            let ctxW = document.getElementById('waveformCanvas').getContext('2d');
            ctxW.fillStyle = '#000'; ctxW.fillRect(0,0,800,150);
            ctxW.strokeStyle = '#00e676'; ctxW.lineWidth = 2;
            ctxW.beginPath();
            let sliceWidth = 800 * 1.0 / dataArray.length;
            let x = 0;
            let rms = 0;
            for(let i=0; i<dataArray.length; i++) {
                let v = dataArray[i] / 128.0;
                let y = v * 150 / 2;
                if(i===0) ctxW.moveTo(x, y); else ctxW.lineTo(x, y);
                x += sliceWidth;
                rms += Math.abs(dataArray[i] - 128);
            }
            ctxW.lineTo(800, 150/2);
            ctxW.stroke();
            
            // VU Meter
            document.getElementById('vu-bar').style.width = Math.min(100, (rms/dataArray.length)*8) + "%";
        }
    </script>
</body>
</html>
)rawliteral";

// --- ESP32 核心邏輯 ---

void setupI2S() {
    i2s_config_t i2s_config = {
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
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = -1,
        .data_in_num = I2S_SD
    };
    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
}

void setup() {
    Serial.begin(115200);
    setupI2S();

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected. IP:");
    Serial.println(WiFi.localIP());

    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) Serial.printf("WebSocket client #%u connected\n", client->id());
    });
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    server.begin();
}

void loop() {
    int16_t i2s_buffer[DMA_BUF_LEN];
    size_t bytes_read;
    
    // 從麥克風讀取數據
    esp_err_t result = i2s_read(I2S_PORT, &i2s_buffer, sizeof(i2s_buffer), &bytes_read, portMAX_DELAY);

    if (result == ESP_OK && bytes_read > 0) {
        // 如果網頁有人連線，將數據廣播出去
        if (ws.count() > 0) {
            ws.binaryAll((uint8_t*)i2s_buffer, bytes_read);
        }
    }
}
