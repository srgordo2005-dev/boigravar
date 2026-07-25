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
#include "AudioOutputI2S.h"

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
#define PIR_PIN     27
#define TOUCH1_PIN  32
#define TOUCH2_PIN  33
#define RELAY_PIN   14
#define RELE_AUX1   4
#define RELE_AUX2   5

#define TEMPO_DESLIGAR_MS 20000
#define DEBOUNCE_MS       1000   
#define TOUCH_THRESHOLD 35 
#define TEMPO_ESPERA_AMPLI 300 

AsyncWebServer server(80);
Preferences prefs;

AudioGeneratorMP3 *mp3;
AudioFileSourceLittleFS *file;
AudioOutputI2S *out;

bool ampliLigado = false;
unsigned long ultimaAtividade = 0;
unsigned long ultimoTouch1 = 0, ultimoTouch2 = 0;
String trackTouch1, trackTouch2;

bool esperandoAmpli = false;
unsigned long tempoInicioAmpli = 0;
String trackPendente = "";

bool isAPMode = false;
bool licenciado = false;
bool bloqueado = false;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}
h2{color:#4fd1c5}
.card{background:#1c1c1c;border-radius:12px;padding:14px;margin-bottom:14px}
select,input[type=file],input[type=range],input[type=text],input[type=password]{width:100%;padding:8px;margin-top:6px;border-radius:8px;border:none;box-sizing:border-box}
button{background:#4fd1c5;border:none;padding:10px 16px;border-radius:8px;font-weight:bold;margin-top:8px;cursor:pointer}
.btn-play{background:#2ecc71;padding:6px 12px;margin-left:10px}
.btn-red{background:#e74c3c}
li{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #333}
.acoes{display:flex;gap:10px}
a{color:#ff6b6b;text-decoration:none}
small{color:#888}
</style></head><body>
<h2>Painel do Boi (Touch Interno)</h2>
<div class="card"><b>Configurar Wi-Fi e Nome</b><br>
<form action="/savewifi" method="GET">
<input type="text" name="bname" placeholder="Nome do Boi (Ex: Boi 01)" required>
<input type="text" name="ssid" placeholder="Nome da Rede (SSID)" required>
<input type="password" name="pass" placeholder="Senha da Rede">
<button type="submit">Salvar e Conectar</button>
</form>
</div>
<div class="card"><b>Controles Extras (Reles)</b><br>
<button id="r1" onclick="tgR(1)">Ligar Rele 1 (Pino 4)</button>
<button id="r2" onclick="tgR(2)">Ligar Rele 2 (Pino 5)</button>
</div>
<div class="card"><b>Enviar novo audio</b><br><small>So MP3 curto (mono, 64kbps)</small><br>
<input type="file" id="f" accept=".mp3"><button onclick="up()">Enviar</button>
<div id="prog"></div></div>
<div class="card"><b>Faixas salvas no Boi</b><ul id="lista"></ul></div>
<div class="card"><b>Tocar no Touch 1</b><select id="t1" onchange="salvar(1)"></select></div>
<div class="card"><b>Tocar no Touch 2</b><select id="t2" onchange="salvar(2)"></select></div>
<div class="card"><b>Volume Geral</b><input type="range" min="0" max="100" id="vol" onchange="setVol(this.value)"></div>
<script>
async function carregar(){
  let r = await fetch('/list'); let arr = await r.json();
  let ul=document.getElementById('lista'); ul.innerHTML='';
  let t1=document.getElementById('t1'), t2=document.getElementById('t2');
  t1.innerHTML='<option value="">Nenhum</option>'; t2.innerHTML='<option value="">Nenhum</option>';
  arr.forEach(n=>{
    ul.innerHTML+=`<li>${n} <div class="acoes"><button class="btn-play" onclick="play('${n}')">▶</button><a href="#" onclick="del('${n}')">apagar</a></div></li>`;
    t1.innerHTML+=`<option value="${n}">${n}</option>`;
    t2.innerHTML+=`<option value="${n}">${n}</option>`;
  });
  let sel = await (await fetch('/getsel')).json();
  t1.value = sel.t1; t2.value = sel.t2;
}
async function up(){
  let f=document.getElementById('f').files[0]; if(!f) return;
  let fd=new FormData(); fd.append('file',f);
  document.getElementById('prog').innerText='Enviando...';
  await fetch('/upload',{method:'POST',body:fd});
  document.getElementById('prog').innerText='Concluido!';
  carregar();
}
async function del(n){ await fetch('/delete?file='+encodeURIComponent(n)); carregar(); }
async function play(n){ await fetch('/play?file='+encodeURIComponent(n)); }
async function salvar(qual){
  let v = document.getElementById('t'+qual).value;
  await fetch(`/select?touch=${qual}&file=${encodeURIComponent(v)}`);
}
async function setVol(v){ await fetch('/volume?v='+v); }
async function tgR(id){
  let b = document.getElementById('r'+id);
  let state = b.innerText.includes('Ligar') ? 1 : 0;
  await fetch(`/relay?id=${id}&st=${state}`);
  b.innerText = state ? `Desligar Rele ${id}` : `Ligar Rele ${id}`;
  b.className = state ? 'btn-red' : '';
}
carregar();
</script></body></html>
)HTML";

void tocarAgora(String nomeArquivo) {
  if (!licenciado || bloqueado) return; // Sistema de Licença
  if (nomeArquivo == "") return;
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
  if (!licenciado || bloqueado) return; // Sistema de Licença
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

  server.on("/getsel", HTTP_GET, [](AsyncWebServerRequest *r){
    r->send(200, "application/json", "{\"t1\":\"" + trackTouch1 + "\",\"t2\":\"" + trackTouch2 + "\"}");
  });

  server.on("/select", HTTP_GET, [](AsyncWebServerRequest *r){
    int touch = r->getParam("touch")->value().toInt();
    String file = r->getParam("file")->value();
    if (touch == 1) { trackTouch1 = file; prefs.putString("t1", file); }
    else { trackTouch2 = file; prefs.putString("t2", file); }
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

  server.on("/relay", HTTP_GET, [](AsyncWebServerRequest *r){
    int id = r->getParam("id")->value().toInt();
    int st = r->getParam("st")->value().toInt();
    if(id == 1) digitalWrite(RELE_AUX1, st ? HIGH : LOW);
    if(id == 2) digitalWrite(RELE_AUX2, st ? HIGH : LOW);
    r->send(200, "text/plain", "ok");
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
  
  pinMode(RELE_AUX1, OUTPUT);
  pinMode(RELE_AUX2, OUTPUT);
  digitalWrite(RELE_AUX1, LOW);
  digitalWrite(RELE_AUX2, LOW);

  LittleFS.begin(true);

  out = new AudioOutputI2S(0, AudioOutputI2S::INTERNAL_DAC);
  out->SetOutputModeMono(true);
  mp3 = new AudioGeneratorMP3();

  prefs.begin("audio", false);
  trackTouch1 = prefs.getString("t1", "");
  trackTouch2 = prefs.getString("t2", "");
  licenciado = prefs.getBool("lic", false);
  bloqueado = prefs.getBool("blq", false);
  
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
    Serial.println("Iniciando rede do Boi...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("BOI-AUDIO-CONFIG", "12345678"); 
    isAPMode = true;
    Serial.print("Rede AP criada! IP: ");
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

    if (touchRead(TOUCH1_PIN) < TOUCH_THRESHOLD && millis() - ultimoTouch1 > DEBOUNCE_MS) {
      ultimoTouch1 = millis();
      if (trackTouch1 != "") { ligarAmpliETocar(trackTouch1); }
    }

    if (touchRead(TOUCH2_PIN) < TOUCH_THRESHOLD && millis() - ultimoTouch2 > DEBOUNCE_MS) {
      ultimoTouch2 = millis();
      if (trackTouch2 != "") { ligarAmpliETocar(trackTouch2); }
    }
  }

  desligarAmpliSeOcioso();
}
