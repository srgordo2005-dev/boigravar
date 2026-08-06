/*
  ESP32 - Cabeça de Boi (Amplificador + Alto-falante 30W)
  VERSÃO: TOUCH INTERNO + mDNS + TELEGRAM + BOTOES EXTRAS
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "AudioFileSourceLittleFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputNoOp.h"

// ==========================================
// CONFIGURAÇÕES DO TELEGRAM
// ==========================================
#define BOT_TOKEN "8910441602:AAEThz37znPwdX3tUoacqfyIOPT3xnElU6c"
#define CHAT_ID "5651208708"

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
unsigned long lastBotCheck = 0;

// ==========================================
// PINOS
// ==========================================
#define PIR_PIN     4
#define TOUCH1_PIN  1
#define TOUCH2_PIN  2
#define RELAY_PIN   14
#define BOTAO1_PIN  4
#define BOTAO2_PIN  5

#define TEMPO_DESLIGAR_MS 20000
#define DEBOUNCE_MS       1000   
#define TOUCH_THRESHOLD 35 
#define TEMPO_ESPERA_AMPLI 300 

AsyncWebServer server(80);
Preferences prefs;

AudioGeneratorMP3 *mp3;
AudioFileSourceLittleFS *file;
AudioOutputNoOp *out;

bool ampliLigado = false;
unsigned long ultimaAtividade = 0;
unsigned long ultimoTouch1 = 0, ultimoTouch2 = 0;
unsigned long ultimoBotao1 = 0, ultimoBotao2 = 0;
String trackTouch1, trackTouch2;
String trackBotao1, trackBotao2;

bool esperandoAmpli = false;
unsigned long tempoInicioAmpli = 0;
String trackPendente = "";

bool isAPMode = false;
bool licenciado = false;
bool bloqueado = false;
int offline_plays = 0;

String scanJson = "[]";

int baseTouch1 = 50;
int baseTouch2 = 50;
const int offsetSensibilidade = 15; // O quanto a leitura tem que cair para considerar um toque

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}
h2{color:#4fd1c5; margin-bottom:5px;}
#status{padding:10px;border-radius:8px;font-weight:bold;margin-bottom:14px;text-align:center;}
.liberado{background:#2ecc71;color:#000} .aguardando{background:#f1c40f;color:#000} .bloqueado{background:#e74c3c;color:#fff}
.card{background:#1c1c1c;border-radius:12px;padding:14px;margin-bottom:14px}
select,input[type=file],input[type=range],input[type=text],input[type=password]{width:100%;padding:8px;margin-top:6px;border-radius:8px;border:none;box-sizing:border-box}
button{background:#4fd1c5;border:none;padding:10px 16px;border-radius:8px;font-weight:bold;margin-top:8px;cursor:pointer;color:#000}
.btn-play{background:#2ecc71;padding:6px 12px;margin-left:10px}
li{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #333}
.acoes{display:flex;gap:10px}
a{color:#ff6b6b;text-decoration:none}
small{color:#888}
</style></head><body>
<h2>Painel do Boi</h2>
<div id="status" class="aguardando">Carregando status...</div>

<div class="card"><b>Configurar Wi-Fi e Nome</b><br>
<form action="/savewifi" method="GET">
<input type="text" id="iptNome" name="bname" placeholder="Nome do Boi (Ex: Boi 01)" required>
<input type="text" name="ssid" list="redes" placeholder="Nome da Rede (SSID)" required autocomplete="off">
<datalist id="redes"></datalist>
<input type="password" name="pass" placeholder="Senha da Rede">
<button type="submit">Salvar e Conectar</button>
</form>
</div>

<div class="card"><b>Enviar novo audio</b><br><small>So MP3 curto (mono, 64kbps)</small><br>
<input type="file" id="f" accept=".mp3"><button onclick="up()">Enviar</button>
<div id="prog"></div></div>

<div class="card"><b>Monitor de Touch (Ao Vivo)</b><br>
<small>Valores no S3 aumentam ao encostar. T1 e T2 são os pinos 1 e 2.</small>
<div style="font-size:20px; margin-top:10px; color:#f1c40f;">Touch 1: <span id="valT1">--</span></div>
<div style="font-size:20px; margin-top:5px; color:#f1c40f;">Touch 2: <span id="valT2">--</span></div>
</div>

<div class="card"><b>Faixas salvas no Boi</b><ul id="lista"></ul></div>

<div class="card"><b>Calibração do Touch Interno</b><br>
<button onclick="calib()" style="background:#f39c12; width:100%;">Calibrar Sensibilidade</button>
</div>

<div class="card">
<b>Atalhos de Áudio</b><br><br>
Tocar no Touch 1<select id="t1" onchange="salvar('t1')"></select><br>
Tocar no Touch 2<select id="t2" onchange="salvar('t2')"></select><br>
Tocar no Botão 1 (Pino 4)<select id="b1" onchange="salvar('b1')"></select><br>
Tocar no Botão 2 (Pino 5)<select id="b2" onchange="salvar('b2')"></select>
</div>
<div class="card"><b>Volume Geral</b><input type="range" min="0" max="100" id="vol" onchange="setVol(this.value)"></div>

<script>
async function carregar(){
  let sR = await fetch('/status'); let sJ = await sR.json();
  let st = document.getElementById('status');
  if(sJ.blq){ st.className='bloqueado'; st.innerText='🔴 TRAVADO ('+sJ.offp+'/100 offline plays)'; }
  else if(sJ.lic){ st.className='liberado'; st.innerText='🟢 LIBERADO ('+sJ.offp+'/100 offline plays)'; }
  else{ st.className='aguardando'; st.innerText='🟡 AGUARDANDO LIBERAÇÃO'; }
  
  if(sJ.bname && sJ.bname != "Boi sem nome"){
    document.getElementById('iptNome').style.display = 'none';
    document.getElementById('iptNome').value = sJ.bname;
  }

  let r = await fetch('/list'); let arr = await r.json();
  let ul=document.getElementById('lista'); ul.innerHTML='';
  let ids=['t1','t2','b1','b2'];
  ids.forEach(id => { document.getElementById(id).innerHTML='<option value="">Nenhum</option>'; });
  
  arr.forEach(n=>{
    ul.innerHTML+=`<li>${n} <div class="acoes"><button class="btn-play" onclick="play('${n}')">▶</button><a href="#" onclick="del('${n}')">apagar</a></div></li>`;
    ids.forEach(id => { document.getElementById(id).innerHTML+=`<option value="${n}">${n}</option>`; });
  });
  
  let sel = await (await fetch('/getsel')).json();
  document.getElementById('t1').value = sel.t1; document.getElementById('t2').value = sel.t2;
  document.getElementById('b1').value = sel.b1; document.getElementById('b2').value = sel.b2;

  try {
    let rs = await fetch('/scan'); let rArr = await rs.json();
    let dl = document.getElementById('redes');
    rArr.forEach(s => { dl.innerHTML += `<option value="${s}">`; });
  } catch(e){}
}
async function getLiveTouch(){
  try {
    let r = await fetch('/touch_status');
    let j = await r.json();
    document.getElementById('valT1').innerText = j.t1;
    document.getElementById('valT2').innerText = j.t2;
  } catch(e){}
}
setInterval(getLiveTouch, 500);

async function up(){
  let f=document.getElementById('f').files[0]; if(!f) return;
  let fd=new FormData(); fd.append('file',f);
  document.getElementById('prog').innerText='Enviando...';
  await fetch('/upload',{method:'POST',body:fd});
  document.getElementById('prog').innerText='Concluido!';
  carregar();
}
async function calib(){
  document.getElementById('prog').innerText='Calibrando...';
  await fetch('/calib');
  alert('Calibrado com sucesso!');
  document.getElementById('prog').innerText='';
}
async function del(n){ await fetch('/delete?file='+encodeURIComponent(n)); carregar(); }
async function play(n){ await fetch('/play?file='+encodeURIComponent(n)); }
async function salvar(qual){
  let v = document.getElementById(qual).value;
  await fetch(`/select?qual=${qual}&file=${encodeURIComponent(v)}`);
}
async function setVol(v){ await fetch('/volume?v='+v); }
carregar();
</script></body></html>
)HTML";

void tocarAgora(String nomeArquivo) {
  if (nomeArquivo == "") return;
  if (!licenciado || bloqueado) return;
  
  if (WiFi.status() == WL_CONNECTED) {
    if (offline_plays > 0) { offline_plays = 0; prefs.putInt("offp", 0); }
  } else {
    offline_plays++;
    prefs.putInt("offp", offline_plays);
    if (offline_plays >= 100) {
      if (mp3->isRunning()) mp3->stop();
      if (file) delete file;
      file = new AudioFileSourceLittleFS("/desconectado.mp3");
      if(file->isOpen()) { mp3->begin(file, out); ultimaAtividade = millis(); }
      return; 
    }
  }

  if (mp3->isRunning()) mp3->stop();
  if (file) delete file;
  
  file = new AudioFileSourceLittleFS(("/" + nomeArquivo).c_str());
  if(file->isOpen()){
    mp3->begin(file, out);
    ultimaAtividade = millis();
  } else {
    delete file;
    file = nullptr;
  }
}

void ligarAmpliETocar(String track) {
  if (track == "") return;
  if (!licenciado || bloqueado) return;
  ultimaAtividade = millis();
  if (!ampliLigado) {
    digitalWrite(RELAY_PIN, HIGH);
    ampliLigado = true;
    esperandoAmpli = true;
    tempoInicioAmpli = millis();
    trackPendente = track;
  } else {
    tocarAgora(track);
  }
}

void desligarAmpliSeOcioso() {
  if (ampliLigado && !mp3->isRunning() && !esperandoAmpli && millis() - ultimaAtividade > TEMPO_DESLIGAR_MS) {
    digitalWrite(RELAY_PIN, LOW);
    ampliLigado = false;
  }
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    if (chat_id != CHAT_ID) continue; // Segurança
    
    String text = bot.messages[i].text;
    String mac = WiFi.macAddress();
    
    if (text == "/liberar " + mac) {
      licenciado = true;
      prefs.putBool("lic", true);
      bot.sendMessage(chat_id, "✅ Boi liberado e registrado com sucesso!", "");
    }
    else if (text == "/travar " + mac) {
      bloqueado = true;
      prefs.putBool("blq", true);
      bot.sendMessage(chat_id, "🔒 Boi travado com sucesso! Ele não fará mais barulhos.", "");
    }
    else if (text == "/destravar " + mac) {
      bloqueado = false;
      prefs.putBool("blq", false);
      bot.sendMessage(chat_id, "🔓 Boi destravado com sucesso!", "");
    }
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", INDEX_HTML); });
  
  server.on("/savewifi", HTTP_GET, [](AsyncWebServerRequest *r){
    String n = r->hasParam("bname") ? r->getParam("bname")->value() : "Boi sem nome";
    String s = r->getParam("ssid")->value();
    String p = r->hasParam("pass") ? r->getParam("pass")->value() : "";
    prefs.putString("bname", n);
    prefs.putString("ssid", s);
    prefs.putString("pass", p);
    r->send(200, "text/html", "<h2>Configuracao salva! O Boi vai reiniciar agora.</h2>");
    delay(2000);
    ESP.restart();
  });

  server.on("/list", HTTP_GET, [](AsyncWebServerRequest *r){
    String json = "[";
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    bool first = true;
    while (f) {
      if (!f.isDirectory() && String(f.name()).endsWith(".mp3")) {
        if (!first) json += ",";
        json += "\"" + String(f.name()) + "\"";
        first = false;
      }
      f = root.openNextFile();
    }
    json += "]";
    r->send(200, "application/json", json);
  });

  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(200, "application/json", scanJson);
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r){
    String bname = prefs.getString("bname", "");
    String json = "{\"lic\":" + String(licenciado ? "true" : "false") + 
                  ",\"blq\":" + String(bloqueado ? "true" : "false") + 
                  ",\"offp\":" + String(offline_plays) + 
                  ",\"bname\":\"" + bname + "\"}";
    r->send(200, "application/json", json);
  });

  server.on("/getsel", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(200, "application/json", "{\"t1\":\"" + trackTouch1 + "\",\"t2\":\"" + trackTouch2 + "\",\"b1\":\"" + trackBotao1 + "\",\"b2\":\"" + trackBotao2 + "\"}");
  });

  server.on("/select", HTTP_GET, [](AsyncWebServerRequest *r){
    String qual = r->getParam("qual")->value();
    String file = r->getParam("file")->value();
    if (qual == "t1") { trackTouch1 = file; prefs.putString("t1", file); }
    else if (qual == "t2") { trackTouch2 = file; prefs.putString("t2", file); }
    else if (qual == "b1") { trackBotao1 = file; prefs.putString("b1", file); }
    else if (qual == "b2") { trackBotao2 = file; prefs.putString("b2", file); }
    r->send(200, "text/plain", "ok");
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *r){
    String file = r->getParam("file")->value();
    LittleFS.remove("/" + file);
    r->send(200, "text/plain", "ok");
  });

  server.on("/volume", HTTP_GET, [](AsyncWebServerRequest *r){
    int v = r->getParam("v")->value().toInt();
    out->SetGain(v / 100.0);
    r->send(200, "text/plain", "ok");
  });
  
  server.on("/play", HTTP_GET, [](AsyncWebServerRequest *r){
    String file = r->getParam("file")->value();
    ligarAmpliETocar(file);
    r->send(200, "text/plain", "ok");
  });

  server.on("/calib", HTTP_GET, [](AsyncWebServerRequest *r){
    int t1 = 0, t2 = 0;
    for(int i=0; i<10; i++){ t1 += touchRead(TOUCH1_PIN); t2 += touchRead(TOUCH2_PIN); delay(10); }
    baseTouch1 = t1 / 10;
    baseTouch2 = t2 / 10;
    prefs.putInt("bT1", baseTouch1);
    prefs.putInt("bT2", baseTouch2);
    r->send(200, "text/plain", "ok");
  });

  server.on("/touch_status", HTTP_GET, [](AsyncWebServerRequest *r){
    String json = "{\"t1\":";
    json += touchRead(TOUCH1_PIN);
    json += ",\"t2\":";
    json += touchRead(TOUCH2_PIN);
    json += "}";
    r->send(200, "application/json", json);
  });

  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *r){ r->send(200); },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
      static File up;
      if (index == 0) up = LittleFS.open("/" + filename, "w");
      if (up) up.write(data, len);
      if (final && up) up.close();
    });

  server.begin();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(BOTAO1_PIN, INPUT_PULLUP);
  pinMode(BOTAO2_PIN, INPUT_PULLUP);

  LittleFS.begin(true);

  // S3 não tem DAC. Para não travar a placa aguardando saída I2S, usamos saída fantasma
  out = new AudioOutputNoOp();
  mp3 = new AudioGeneratorMP3();

  prefs.begin("audio", false);
  trackTouch1 = prefs.getString("t1", "");
  trackTouch2 = prefs.getString("t2", "");
  trackBotao1 = prefs.getString("b1", "");
  trackBotao2 = prefs.getString("b2", "");
  
  licenciado = prefs.getBool("lic", false);
  bloqueado = prefs.getBool("blq", false);
  offline_plays = prefs.getInt("offp", 0);
  baseTouch1 = prefs.getInt("bT1", 50);
  baseTouch2 = prefs.getInt("bT2", 50);
  
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");

  secured_client.setInsecure(); // Necessario para o Telegram no ESP32

  bool connected = false;
  if (savedSSID != "") {
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    Serial.print("Conectando ao WiFi");
    
    unsigned long startTry = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTry < 10000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.print("Conectado! IP: ");
      Serial.println(WiFi.localIP());
      
      // Inicia mDNS (Acesso via http://boi.local)
      if (MDNS.begin("boi")) {
        Serial.println("MDNS ativo! Acesse http://boi.local");
      }

      ligarAmpliETocar("conectado.mp3");

      // Telegram: Envia mensagem se não estiver licenciado ainda
      if (!licenciado) {
         String bname = prefs.getString("bname", "Sem Nome");
         String msg = "🐂 NOVO BOI CONECTADO!\nNome: " + bname + "\nMAC: " + WiFi.macAddress() + "\nIP Local: " + WiFi.localIP().toString() + "\n\nPara liberar envie:\n/liberar " + WiFi.macAddress();
         bot.sendMessage(CHAT_ID, msg, "");
      }
    }
  }

  if (!connected) {
    Serial.println("Iniciando rede do Boi e Escaneando Wi-Fi...");
    WiFi.mode(WIFI_AP_STA);
    
    int n = WiFi.scanNetworks();
    scanJson = "[";
    for (int i = 0; i < n; ++i) {
      if (i > 0) scanJson += ",";
      scanJson += "\"" + WiFi.SSID(i) + "\"";
    }
    scanJson += "]";

    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String apName = "BOI-AUDIO-" + mac.substring(mac.length() - 4);
    apName.toUpperCase();

    WiFi.softAP(apName.c_str(), "12345678"); 
    isAPMode = true;
    Serial.print("Rede AP criada: ");
    Serial.print(apName);
    Serial.print(" | IP: ");
    Serial.println(WiFi.softAPIP());
    ligarAmpliETocar("desconectado.mp3");
  }

  setupWebServer();
}

void loop() {
  if (mp3->isRunning()) { 
    if (!mp3->loop()) mp3->stop(); 
  } else {
    // Checa mensagens do Telegram apenas quando não estiver tocando som (para não travar)
    if (millis() - lastBotCheck > 5000) {
       int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
       while (numNewMessages) {
          handleNewMessages(numNewMessages);
          numNewMessages = bot.getUpdates(bot.last_message_received + 1);
       }
       lastBotCheck = millis();
    }
  }

  if (esperandoAmpli && (millis() - tempoInicioAmpli >= TEMPO_ESPERA_AMPLI)) {
    esperandoAmpli = false;
    tocarAgora(trackPendente);
    trackPendente = "";
  }

  // Só permite acionamento por sensor se estiver licenciado e destravado
  if (licenciado && !bloqueado) {
    if (digitalRead(PIR_PIN) == HIGH) {
      if (!ampliLigado) {
         ultimaAtividade = millis();
         digitalWrite(RELAY_PIN, HIGH);
         ampliLigado = true;
      }
    }

    if (touchRead(TOUCH1_PIN) < (baseTouch1 - offsetSensibilidade) && millis() - ultimoTouch1 > DEBOUNCE_MS) {
      ultimoTouch1 = millis();
      if (trackTouch1 != "") { ligarAmpliETocar(trackTouch1); }
    }

    if (touchRead(TOUCH2_PIN) < (baseTouch2 - offsetSensibilidade) && millis() - ultimoTouch2 > DEBOUNCE_MS) {
      ultimoTouch2 = millis();
      if (trackTouch2 != "") { ligarAmpliETocar(trackTouch2); }
    }

    if (digitalRead(BOTAO1_PIN) == LOW && millis() - ultimoBotao1 > DEBOUNCE_MS) {
      ultimoBotao1 = millis();
      if (trackBotao1 != "") { ligarAmpliETocar(trackBotao1); }
    }

    if (digitalRead(BOTAO2_PIN) == LOW && millis() - ultimoBotao2 > DEBOUNCE_MS) {
      ultimoBotao2 = millis();
      if (trackBotao2 != "") { ligarAmpliETocar(trackBotao2); }
    }
  }

  desligarAmpliSeOcioso();
}
