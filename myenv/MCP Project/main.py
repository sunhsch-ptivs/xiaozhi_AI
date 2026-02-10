import tkinter as tk
from tkinter import scrolledtext
import websocket
import json
import threading
import time
import random
import requests
import yfinance as yf

# ==============================================================================
# ★★★ 請在此填入你的最新 Token ★★★
MCP_URL = "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjQwNzc5MywiYWdlbnRJZCI6MTI0NDc0NSwiZW5kcG9pbnRJZCI6ImFnZW50XzEyNDQ3NDUiLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzY2ODk0OTgzLCJleHAiOjE3OTg0NTI1ODN9.VPTzoposijS7v2vAzPkTVo6uIPVW1xZ4jwxwg1YXNjcYcECnnYbetW4xsoeqR9l-WGB8tZPMV8NuBsDtIr4S0g"
# ==============================================================================
# ★★★ Tasmota 檯燈 IP ★★★
TASMOTA_IP = "192.168.0.238"
# ==============================================================================

class YouBikeAPIClient:
    def __init__(self, logger=None):
        self.api_url = "https://tcgbusfs.blob.core.windows.net/dotapp/youbike/v2/youbike_immediate.json"
        self.logger = logger
        
        # 內建簡易「簡體 -> 繁體」對照表
        self.s2t_map = str.maketrans({
            '运': '運', '馆': '館', '区': '區', '号': '號', '园': '園',
            '场': '場', '兴': '興', '体': '體', '育': '育', '路': '路',
            '街': '街', '电': '電', '湾': '灣', '学': '學', '里': '里',
            '宫': '宮', '东': '東', '南': '南', '西': '西', '北': '北',
            '关': '關', '会': '會', '员': '員', '从': '從', '众': '眾',
            '业': '業', '书': '書', '画': '畫', '气': '氣', '温': '溫',
            '国': '國', '华': '華', '宝': '寶', '楼': '樓', '医': '醫',
            '龙': '龍', '龟': '龜', '戏': '戲', '联': '聯', '点': '點',
            '务': '務', '时': '時', '机': '機', '车': '車', '站': '站',
            '台': '台', '臺': '台' # 強制統一
        })

    def log(self, msg):
        if self.logger: self.logger("UBIKE", msg)
        else: print(f"[UBIKE] {msg}")

    def normalize_text(self, text):
        # 1. 簡轉繁
        text = text.translate(self.s2t_map)
        # 2. 強制將「臺」轉為「台」，忽略大小寫
        return text.replace("臺", "台").lower()

    def search(self, city_name, keyword):
        try:
            self.log(f"正在請求 API: {self.api_url}")
            r = requests.get(self.api_url, timeout=10)
            if r.status_code != 200: 
                return {"error": f"API Error {r.status_code}"}
            
            all_stations = r.json()
            self.log(f"API 回傳成功，共 {len(all_stations)} 筆")
            
            # 處理搜尋關鍵字 (正規化 + 分詞)
            keyword_norm = self.normalize_text(keyword)
            search_terms = keyword_norm.split() # 支援空格分隔，如 "公館 2"
            
            self.log(f"搜尋邏輯: {search_terms} (原始: '{keyword}')")
            
            results = []
            for st in all_stations:
                raw_name = st.get('sna', '')
                display_name = raw_name.replace("YouBike2.0_", "")
                
                # 參考你提供的教學文章：檢查字串是否包含搜尋內容
                # 這裡我們加上 normalize_text 來解決「台/臺」與「簡體」問題
                check_name = self.normalize_text(display_name)
                
                # 智慧比對：所有關鍵字都要存在 (AND 邏輯)
                if all(term in check_name for term in search_terms):
                    results.append({
                        "name": display_name,
                        "bikes": st.get('available_rent_bikes', 0),
                        "spaces": st.get('available_return_bikes', 0),
                        "city": "台北市",
                        "raw_data": st
                    })
            
            # ★★★ 優化重點：回傳所有結果，而不只是第一個 ★★★
            if results:
                self.log(f"搜尋成功，找到 {len(results)} 筆相符資料")
                # 限制回傳筆數，避免塞爆 Log 或 Token，這裡取前 10 筆
                return results[:10]
            else:
                self.log("搜尋結果: 無匹配")
                return []

        except Exception as e: 
            self.log(f"例外錯誤: {str(e)}")
            return {"error": str(e)}

class XiaozhiMCPSimulator:
    def __init__(self, root, url):
        self.root = root
        self.url = url
        self.root.title("ESP32 Digital Twin (List Search)")
        self.root.geometry("950x1080")
        
        self.led_state = False 
        self.sim_temp = 25.0       
        self.tasmota_status = {"power": "UNKNOWN", "dimmer": 0, "ct": 153}
        self.stock_info = {"2330": "載入中...", "2454": "載入中..."}
        
        self.effect_running = False 
        self.ws = None
        self.keep_running = True 
        self.is_dragging = False
        
        self.setup_ui()
        self.ubike_client = YouBikeAPIClient(logger=self.log) 
        self.start_sim_sensor() 
        
        threading.Thread(target=self.loop_external_data, daemon=True).start()
        threading.Thread(target=self.forever_connect_loop, daemon=True).start()

    def setup_ui(self):
        # 1. 狀態列
        status_frame = tk.Frame(self.root, pady=10, bg="#111")
        status_frame.pack(fill=tk.X)
        self.lbl_status = tk.Label(status_frame, text="System Ready", fg="#00FF00", bg="#111", font=("Consolas", 12, "bold"))
        self.lbl_status.pack()

        # 2. 儀表板
        dashboard_frame = tk.Frame(self.root, padx=10, pady=10)
        dashboard_frame.pack(fill=tk.X)

        # [Row 0] 既有面板
        panel_device = tk.LabelFrame(dashboard_frame, text="WS2812 RGB LED", font=("Microsoft JhengHei", 10, "bold"), padx=15, pady=15)
        panel_device.grid(row=0, column=0, padx=5, pady=5, sticky="nsew")
        
        self.canvas_led = tk.Canvas(panel_device, width=80, height=80)
        self.canvas_led.pack(side=tk.LEFT)
        self.led_glow = self.canvas_led.create_oval(5, 5, 75, 75, fill="", outline="", width=0)
        self.led_circle = self.canvas_led.create_oval(15, 15, 65, 65, fill="#555555", outline="black", width=2)
        
        frame_led_info = tk.Frame(panel_device)
        frame_led_info.pack(side=tk.LEFT, padx=10)
        self.lbl_led_mode = tk.Label(frame_led_info, text="Mode: OFF", font=("Consolas", 10, "bold"))
        self.lbl_led_mode.pack(anchor="w")
        self.lbl_led_hex = tk.Label(frame_led_info, text="#000000", font=("Consolas", 9))
        self.lbl_led_hex.pack(anchor="w")

        # [Row 0] 資訊面板
        panel_info = tk.LabelFrame(dashboard_frame, text="市場監控", font=("Microsoft JhengHei", 10, "bold"), padx=15, pady=15)
        panel_info.grid(row=0, column=1, padx=5, pady=5, sticky="nsew")
        self.lbl_sim_temp = tk.Label(panel_info, text="Temp: 25.0°C", font=("Arial", 14, "bold"), fg="blue")
        self.lbl_sim_temp.pack(anchor="w")
        self.lbl_weather = tk.Label(panel_info, text="台北: --", font=("Microsoft JhengHei", 10))
        self.lbl_weather.pack(anchor="w")
        
        tk.Label(panel_info, text="-------- 固定監控 --------", fg="#888").pack(pady=2)
        self.lbl_stock = tk.Label(panel_info, text="載入中...", font=("Microsoft JhengHei", 9), justify=tk.LEFT, fg="#D32F2F")
        self.lbl_stock.pack(anchor="w")

        tk.Label(panel_info, text="-------- 即時查詢 --------", fg="#888").pack(pady=2)
        self.lbl_stock_search = tk.Label(panel_info, text="等待查詢...", font=("Microsoft JhengHei", 9), justify=tk.LEFT, fg="#2E7D32")
        self.lbl_stock_search.pack(anchor="w")

        # [Row 0] YouBike
        panel_ubike = tk.LabelFrame(dashboard_frame, text="YouBike (v2.0)", font=("Microsoft JhengHei", 10, "bold"), padx=15, pady=15)
        panel_ubike.grid(row=0, column=2, padx=5, pady=5, sticky="nsew")
        self.lbl_ub_station = tk.Label(panel_ubike, text="站點: --", font=("Microsoft JhengHei", 11, "bold"), fg="#006600")
        self.lbl_ub_station.pack(anchor="w")
        self.lbl_ub_info = tk.Label(panel_ubike, text="狀態: --", font=("Microsoft JhengHei", 10))
        self.lbl_ub_info.pack(anchor="w")

        # [Row 1] Tasmota 控制區
        panel_tasmota = tk.LabelFrame(dashboard_frame, text=f"Tasmota 智慧檯燈 ({TASMOTA_IP})", font=("Microsoft JhengHei", 10, "bold"), padx=20, pady=15, fg="#E65100")
        panel_tasmota.grid(row=1, column=0, columnspan=3, padx=5, pady=10, sticky="nsew")

        frame_power = tk.Frame(panel_tasmota)
        frame_power.pack(side=tk.LEFT, padx=10)
        self.lbl_tas_power = tk.Label(frame_power, text="OFF", font=("Arial", 20, "bold"), fg="gray")
        self.lbl_tas_power.pack()
        btn_toggle = tk.Button(frame_power, text="電源切換", command=self.manual_toggle_tasmota, bg="#FFB74D")
        btn_toggle.pack(pady=5)

        frame_dimmer = tk.Frame(panel_tasmota)
        frame_dimmer.pack(side=tk.LEFT, padx=30, fill=tk.X, expand=True)
        tk.Label(frame_dimmer, text="亮度 (Dimmer)", font=("Microsoft JhengHei", 10)).pack(anchor="w")
        self.scale_dimmer = tk.Scale(frame_dimmer, from_=0, to=100, orient=tk.HORIZONTAL, length=200, showvalue=1)
        self.scale_dimmer.pack()
        self.scale_dimmer.bind("<ButtonRelease-1>", self.on_dimmer_change)
        self.scale_dimmer.bind("<Button-1>", lambda e: self.set_dragging(True))

        frame_ct = tk.Frame(panel_tasmota)
        frame_ct.pack(side=tk.LEFT, padx=30, fill=tk.X, expand=True)
        tk.Label(frame_ct, text="色溫 (CT) [153暖 ←→ 500冷]", font=("Microsoft JhengHei", 10)).pack(anchor="w")
        self.scale_ct = tk.Scale(frame_ct, from_=153, to=500, orient=tk.HORIZONTAL, length=200, showvalue=1)
        self.scale_ct.pack()
        self.scale_ct.bind("<ButtonRelease-1>", self.on_ct_change)
        self.scale_ct.bind("<Button-1>", lambda e: self.set_dragging(True))

        # 3. Log
        log_frame = tk.LabelFrame(self.root, text="MCP Log", padx=5, pady=5)
        log_frame.pack(pady=10, padx=10, fill=tk.BOTH, expand=True)
        self.txt_log = scrolledtext.ScrolledText(log_frame, bg="black", fg="#00FF00", font=("Consolas", 9))
        self.txt_log.pack(fill=tk.BOTH, expand=True)

    def log(self, tag, msg):
        self.root.after(0, lambda: self._write_log(f"[{time.strftime('%H:%M:%S')}] {tag}: {msg}\n"))
    def _write_log(self, msg):
        self.txt_log.insert(tk.END, msg); self.txt_log.see(tk.END)

    def set_dragging(self, state):
        self.is_dragging = state

    # --- 股市資料抓取 ---
    def get_stock_detail(self, symbol):
        if symbol.isdigit(): symbol = f"{symbol}.TW"
        try:
            tick = yf.Ticker(symbol)
            hist = tick.history(period="5d")
            if hist.empty: return None
            last = hist.iloc[-1]
            return {
                "symbol": symbol.replace(".TW", ""),
                "price": last["Close"],
                "open": last["Open"],
                "high": last["High"],
                "low": last["Low"],
                "vol": int(last["Volume"] / 1000)
            }
        except: return None

    def format_stock_str(self, data):
        if not data: return "讀取失敗"
        return f"{data['symbol']}: {data['price']:.2f} (開:{data['open']:.2f} 高:{data['high']:.2f} 低:{data['low']:.2f} 量:{data['vol']}張)"

    # --- Tasmota 核心 ---
    def on_dimmer_change(self, event):
        self.set_dragging(False)
        threading.Thread(target=self.control_tasmota, args=(f"Dimmer {self.scale_dimmer.get()}",)).start()

    def on_ct_change(self, event):
        self.set_dragging(False)
        threading.Thread(target=self.control_tasmota, args=(f"CT {self.scale_ct.get()}",)).start()

    def fetch_tasmota_status(self):
        if self.is_dragging: return
        try:
            url = f"http://{TASMOTA_IP}/cm"
            r = requests.get(url, params={'cmnd': 'State'}, timeout=3)
            if r.status_code == 200:
                data = r.json()
                power = data.get("POWER", "OFF")
                dimmer = data.get("Dimmer", 0)
                ct = data.get("CT", 153)
                self.tasmota_status = {"power": power, "dimmer": dimmer, "ct": ct}
                color = "#4CAF50" if power == "ON" else "gray"
                self.root.after(0, lambda: self.lbl_tas_power.config(text=f"{power}", fg=color))
                self.root.after(0, lambda: self.scale_dimmer.set(dimmer))
                self.root.after(0, lambda: self.scale_ct.set(ct))
        except:
            self.root.after(0, lambda: self.lbl_tas_power.config(text="ERR", fg="red"))

    def control_tasmota(self, cmd):
        try:
            url = f"http://{TASMOTA_IP}/cm"
            self.log("IOT", f"TX -> {cmd}")
            requests.get(url, params={'cmnd': cmd}, timeout=3)
            threading.Timer(0.5, self.fetch_tasmota_status).start()
        except Exception as e: self.log("ERR", f"Control Fail: {e}")

    def manual_toggle_tasmota(self):
        threading.Thread(target=self.control_tasmota, args=("Power TOGGLE",)).start()

    # --- LED & 模擬 ---
    def update_led_display(self, color_hex, mode_text):
        self.canvas_led.itemconfig(self.led_circle, fill=color_hex)
        if color_hex in ["#000000", "#555555", "#333333"]: self.canvas_led.itemconfig(self.led_glow, fill="")
        else: self.canvas_led.itemconfig(self.led_glow, fill=color_hex, stipple="gray50")
        self.lbl_led_mode.config(text=f"Mode: {mode_text}"); self.lbl_led_hex.config(text=color_hex)

    def run_static_color(self, hex_color): self.stop_effect(); self.root.after(0, lambda: self.update_led_display(hex_color, "STATIC"))
    def stop_effect(self): self.effect_running = False; time.sleep(0.1)
    
    def run_rainbow(self):
        self.stop_effect(); self.effect_running = True; threading.Thread(target=self._rainbow_loop, daemon=True).start()
    def _rainbow_loop(self):
        colors = ["#FF0000", "#FFFF00", "#00FF00", "#00FFFF", "#0000FF", "#FF00FF"]
        i = 0
        while self.effect_running:
            self.root.after(0, lambda c=colors[i]: self.update_led_display(c, "RAINBOW"))
            i = (i+1)%len(colors); time.sleep(0.5)

    def start_sim_sensor(self):
        self.sim_temp = round(self.sim_temp + random.uniform(-0.1, 0.1), 2)
        self.root.after(1000, self.start_sim_sensor)

    # --- 外部資料輪詢 ---
    def loop_external_data(self):
        cnt = 0
        while self.keep_running:
            self.fetch_tasmota_status()
            if cnt % 12 == 0: 
                try:
                    r = requests.get("https://wttr.in/Taipei?format=j1", timeout=5)
                    self.root.after(0, lambda: self.lbl_weather.config(text=f"台北:{r.json()['current_condition'][0]['temp_C']}°C"))
                except: pass
                
                s1 = self.get_stock_detail("2330.TW")
                s2 = self.get_stock_detail("2454.TW")
                txt1 = self.format_stock_str(s1)
                txt2 = self.format_stock_str(s2)
                
                self.stock_info["2330"] = txt1
                self.stock_info["2454"] = txt2

                self.root.after(0, lambda: self.lbl_stock.config(text=f"{txt1}\n{txt2}"))
                
            cnt += 1; time.sleep(5) 

    # --- MCP ---
    def forever_connect_loop(self):
        websocket.enableTrace(False)
        while self.keep_running:
            try:
                self.ws = websocket.WebSocketApp(self.url, on_open=self.on_open, on_message=self.on_message, on_error=self.on_error, on_close=self.on_close)
                self.ws.run_forever(ping_interval=5, ping_timeout=None)
            except: pass
            time.sleep(3)

    def on_open(self, ws): self.root.after(0, lambda: self.lbl_status.config(text="● ONLINE", fg="#00FF00")); self.log("MCP", "Connected!")
    def on_error(self, ws, error): pass
    def on_close(self, ws, *args): self.log("MCP", "Closed")

    def on_message(self, ws, message):
        self.log("RX", message)
        try:
            data = json.loads(message); msg_id = data.get("id"); method = data.get("method")

            if method == "initialize":
                self.send_json({"jsonrpc": "2.0", "id": msg_id, "result": {"protocolVersion": "2024-11-05", "capabilities": {"tools": {"listChanged": True}}, "serverInfo": {"name": "ESP32-Pro-Stock-v2.1", "version": "11.1"}}})
            elif method == "ping": self.send_json({"jsonrpc": "2.0", "id": msg_id, "result": {}})
            elif method == "tools/list":
                self.send_json({"jsonrpc": "2.0", "id": msg_id, "result": {"tools": [
                    {"name": "control_led", "description": "控制全彩LED", "inputSchema": {"type": "object", "properties": {"mode": {"type": "string", "enum": ["static", "rainbow", "off"]}, "color": {"type": "string"}}, "required": ["mode"]}},
                    {
                        "name": "control_light",
                        "description": "控制 Tasmota 檯燈 (開關/亮度/色溫)",
                        "inputSchema": {
                            "type": "object",
                            "properties": {
                                "state": {"type": "string", "enum": ["on", "off"]},
                                "brightness": {"type": "integer", "minimum": 0, "maximum": 100},
                                "color_temp": {"type": "integer", "minimum": 153, "maximum": 500, "description": "色溫範圍153(暖)-500(冷)"}
                            },
                            "required": ["state"]
                        }
                    },
                    {"name": "read_status", "description": "讀取狀態", "inputSchema": {"type": "object", "properties": {}}},
                    {"name": "search_youbike", "description": "查YouBike", "inputSchema": {"type": "object", "properties": {"city": {"type": "string"}, "station": {"type": "string"}}, "required": ["city", "station"]}},
                    {"name": "search_stock", "description": "查詢股價", "inputSchema": {"type": "object", "properties": {"stock_id": {"type": "string"}}, "required": ["stock_id"]}}
                ]}})
                self.log("SYS", "✅ Tools Registered")

            elif method == "tools/call":
                name = data.get("params", {}).get("name"); args = data.get("params", {}).get("arguments", {}); res = ""
                
                if name == "control_light":
                    state = args.get("state")
                    bri = args.get("brightness")
                    ct = args.get("color_temp")
                    
                    if state == "off":
                        self.control_tasmota("Backlog Power Off; Dimmer 0; Color 0")
                        res = "檯燈已徹底關閉 (狀態歸零)"
                        self.root.after(0, lambda: self.scale_dimmer.set(0))
                    else:
                        cmds = ["Power On"]
                        if bri is not None: cmds.append(f"Dimmer {bri}")
                        if ct is not None: cmds.append(f"CT {ct}")
                        full_cmd = "Backlog " + ";".join(cmds)
                        self.control_tasmota(full_cmd)
                        
                        if bri: self.root.after(0, lambda: self.scale_dimmer.set(bri))
                        if ct: self.root.after(0, lambda: self.scale_ct.set(ct))
                        res = "檯燈設定完成"

                elif name == "read_status":
                    stock_msg = f"台積電:{self.stock_info['2330']}, 聯發科:{self.stock_info['2454']}"
                    res = f"Temp:{self.sim_temp}°C, Lamp:{self.tasmota_status['power']}, Stocks:[{stock_msg}]"
                elif name == "control_led": 
                    if args.get("mode") == "rainbow": self.run_rainbow()
                    else: self.run_static_color(args.get("color", "#000000"))
                    res = "OK"
                elif name == "search_youbike":
                    # ★★★ 呼叫新的搜尋邏輯，並處理回傳的列表 ★★★
                    results = self.ubike_client.search(args.get("city"), args.get("station"))
                    
                    if isinstance(results, list) and results:
                        # 格式化多筆資料給 AI
                        msg_lines = [f"共找到 {len(results)} 筆，列出前 5 筆:"]
                        for i, r in enumerate(results[:5]):
                            msg_lines.append(f"{i+1}. {r['name']} (🚲借:{r['bikes']} / 🅿️還:{r['spaces']})")
                        res = "\n".join(msg_lines)
                        
                        # UI 只更新第一筆當代表
                        first = results[0]
                        self.root.after(0, lambda: self.lbl_ub_station.config(text=f"站點: {first['name']}"))
                        self.root.after(0, lambda: self.lbl_ub_info.config(text=f"狀態: 借{first['bikes']}/還{first['spaces']}"))
                        
                    elif isinstance(results, dict) and "error" in results:
                        res = f"查詢錯誤: {results['error']}"
                    else:
                        res = "找不到符合的站點，請嘗試其他關鍵字。"
                        
                elif name == "search_stock":
                    sid = args.get("stock_id")
                    data = self.get_stock_detail(sid)
                    if data:
                        res_str = self.format_stock_str(data)
                        self.root.after(0, lambda: self.lbl_stock_search.config(text=res_str, fg="#2E7D32"))
                        res = res_str
                    else: res = f"查無 {sid}"
                
                self.send_json({"jsonrpc": "2.0", "id": msg_id, "result": {"content": [{"type": "text", "text": res}]}})

        except Exception as e: self.log("ERR", str(e))

    def send_json(self, data):
        if self.ws and self.ws.sock and self.ws.sock.connected: self.ws.send(json.dumps(data))

if __name__ == "__main__":
    root = tk.Tk(); XiaozhiMCPSimulator(root, MCP_URL); root.mainloop()