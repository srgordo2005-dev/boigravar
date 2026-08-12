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
#include <ESP8266Audio.h>
#include "AudioFileSourceLittleFS.h"
#include "AudioFileSourcePROGMEM.h"
#include "AudioGeneratorMP3.h"
#include "esp32-hal-dac.h"
#include "audios_progmem.h"

#define DAC_BUF_SIZE 2048
volatile uint8_t dacBuffer[DAC_BUF_SIZE];
volatile int dacHead = 0;
volatile int dacTail = 0;
hw_timer_t * dacTimer = NULL;

void IRAM_ATTR dacTimerIsr() {
  if (dacHead != dacTail) {
    dacWrite(25, dacBuffer[dacTail]);
    dacTail = (dacTail + 1) % DAC_BUF_SIZE;
  }
}

// Classe customizada para usar o DAC interno com Timer de Hardware
class AudioOutputCustomDAC : public AudioOutput {
  uint32_t sampleInterval;
  int dacPin;
public:
  AudioOutputCustomDAC(int pin = 25) { 
    dacPin = pin; 
    sampleInterval = 22; // ~44.1kHz default
  }
  ~AudioOutputCustomDAC() {}
  
  bool begin() override { 
    dacHead = 0; dacTail = 0;
    if(dacTimer == NULL) {
      dacTimer = timerBegin(1000000); // 1MHz clock base (1 microssegundo)
      timerAttachInterrupt(dacTimer, &dacTimerIsr);
    }
    timerAlarm(dacTimer, sampleInterval, true, 0);
    timerStart(dacTimer);
    return true; 
  }
  bool stop() override { 
    if(dacTimer != NULL) { timerStop(dacTimer); }
    dacWrite(dacPin, 128); // Mantém em 1.65V (silêncio absoluto) para evitar chiado de ground loop
    return true; 
  }
  void flush() override { dacHead = 0; dacTail = 0; }
  bool SetRate(int hz) override { 
    sampleInterval = 1000000 / hz; 
    if(dacTimer) { timerAlarm(dacTimer, sampleInterval, true, 0); }
    return true; 
  }
  bool SetChannels(int channels) override { return true; }
  bool SetOutputModeMono(bool mono) { return true; }
  
  bool ConsumeSample(int16_t sample[2]) override {
    int32_t mixed = (sample[0] + sample[1]) / 2; 
    mixed = Amplify(mixed); // Aplica o volume do painel
    uint8_t dacValue = (mixed >> 8) + 128;
    
    int nextHead = (dacHead + 1) % DAC_BUF_SIZE;
    while (nextHead == dacTail) {
      yield(); // Buffer cheio, aguarda a interrupção consumir
    }
    
    dacBuffer[dacHead] = dacValue;
    dacHead = nextHead;
    return true;
  }
};

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
#define NUM_TOUCH   6
const int TOUCH_PINS[NUM_TOUCH] = {32, 33, 13, 15, 2, 12};
#define RELAY_PIN   14
#define BOTAO1_PIN  4
#define BOTAO2_PIN  5

#define TEMPO_DESLIGAR_MS 2000
#define DEBOUNCE_MS       1000   
#define TOUCH_THRESHOLD 35 
#define TEMPO_ESPERA_AMPLI 300 

AsyncWebServer server(80);
Preferences prefs;

AudioGeneratorMP3 *mp3;
AudioFileSource *file = nullptr; // Usaremos ponteiro genérico para suportar PROGMEM e LittleFS
AudioOutputCustomDAC *out;

bool ampliLigado = false;
unsigned long ultimaAtividade = 0;
unsigned long ultimoBotao1 = 0, ultimoBotao2 = 0;

bool esperandoAmpli = false;
unsigned long tempoInicioAmpli = 0;
String trackPendente = "";

bool isAPMode = false;
bool licenciado = false;
bool bloqueado = false;
int offline_plays = 0;

String scanJson = "[]";
unsigned long tempoFimCalibracao = 0;
bool calibrando = false;
unsigned long lastSensoresTime = 0;

int valT[NUM_TOUCH];
int baseTouch[NUM_TOUCH];
int threshDown[NUM_TOUCH];
bool touchHab[NUM_TOUCH];
unsigned long ultimoTouch[NUM_TOUCH];
String trackTouch[NUM_TOUCH];
int toquesValidos[NUM_TOUCH];
uint16_t* calibData[NUM_TOUCH] = {nullptr}; // Array dinâmica para calibração de alta precisão
int calibCount[NUM_TOUCH];
int ultimoMin[NUM_TOUCH];
int ultimoMax[NUM_TOUCH];
bool aguardandoSoltar[NUM_TOUCH];

#define MAX_PLAYLIST 30
String playlist[MAX_PLAYLIST];
int playlistSize = 0;
int currentTrackIndex = 0;

float vol = 1.0;
String bname = "Boi";

void ligarRele() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // 0V = Liga o relé Active Low
}

void desligarRele() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // 3.3V = Desliga o relé (pois VCC agora também é 3.3V)
}

bool emCalibracao = false;
unsigned long inicioCalibracao = 0;
int maxT1 = 0, minT1 = 99999;
int maxT2 = 0, minT2 = 99999;
int somaLeituraT[NUM_TOUCH];
int contLeituraT[NUM_TOUCH];

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
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

<div class="card"><b>Faixas salvas no Boi</b><ul id="lista"></ul></div>

<div class="card"><b>Monitor de Touch (Ao Vivo)</b><br>
<small style="color:#aaa;"><b>Nova Calibração:</b> Solte os fios, deixe o valor estabilizar e clique em Iniciar Calibração (10s).</small>
<div id="touchContainer"></div>
<div style="margin-top:15px; padding:10px; border-radius:8px; text-align:center; font-weight:bold; background:#333; color:#888;" id="indicadorVoz">Boi em silêncio</div>
<button onclick="zerar()" style="background:#e74c3c; width:100%; margin-top:10px;">Resetar Bloqueio Offline</button>
</div>
<div class="card"><b>Calibração do Touch Interno</b><br>
<button id="btnCalib" onclick="calib()" style="background:#f39c12; width:100%;">Iniciar Calibração (10s)</button>
</div>

<div class="card">
<b>Modo Playlist de Áudios (Toca-Tudo)</b><br>
<small style="color:#aaa;">Todos os sensores de toque e o <b>Botão 1 (Pino 4)</b> tocarão a mesma música. Aperte o <b>Botão 2 (Pino 5)</b> fisicamente na placa para pular de faixa, ou escolha manualmente abaixo:</small><br><br>
Áudio Atual: <select id="selTrack" onchange="setTrack(this.value)" style="width:100%; margin-top:5px;"></select>
</div>

<div class="card">
<b>Armazenamento Seguro do Boi (Memória)</b><br>
<div style="background:#222; border-radius:5px; height:20px; width:100%; margin-top:5px; overflow:hidden;">
  <div id="barMemory" style="background:#3498db; height:100%; width:0%; transition: width 0.5s;"></div>
</div>
<small style="color:#aaa;" id="textMemory">Calculando espaço...</small>
</div>

<div class="card"><b>Volume Geral</b><input type="range" min="0" max="100" id="vol" onchange="setVol(this.value)"></div>

<script>
let numFitas = 6;
let pinos = [32, 33, 13, 15, 2, 12];

function buildDOM() {
  let touchHtml = '';
  for(let i=0; i<numFitas; i++){
    touchHtml += `<div style="font-size:20px; margin-top:15px; color:#f1c40f;">
      <label><input type="checkbox" id="enT${i}" onchange="setEn(${i}, this.checked)"> Fita ${i+1} (Pino ${pinos[i]})</label>: <span id="valT${i}">--</span>
      <div style="font-size:14px; color:#888; margin-top:5px;">
        Base (Máx): <input type="number" id="inB${i}" onchange="setBase(${i}, this.value)" style="width:70px; background:#222; color:#fff; border:1px solid #555; padding:3px; margin-right:10px;">
        Gatilho (Mín): <input type="number" id="inTd${i}" onchange="setThresh(${i}, this.value)" style="width:70px; background:#222; color:#fff; border:1px solid #555; padding:3px;">
      </div>
      <small style="color:#666; font-size:12px;" id="calT${i}">Última calibração -> Máx: -- | Mín: --</small>
    </div>`;
  }
  document.getElementById('touchContainer').innerHTML = touchHtml;
}
buildDOM();

async function carregar(){
  let sR = await fetch('/status'); let sJ = await sR.json();
  let st = document.getElementById('status');
  if(sJ.blq){ st.className='bloqueado'; st.innerText='🔴 TRAVADO (Toques esgotados)'; }
  else if(sJ.lic){ st.className='liberado'; st.innerText='🟢 LIBERADO (Restam ' + (100 - sJ.offp) + ' offline plays)'; }
  else{ st.className='aguardando'; st.innerText='🟡 AGUARDANDO LIBERAÇÃO'; }
  
  if(sJ.bname && sJ.bname != "Boi sem nome"){
      document.getElementById('indicadorVoz').style.color = '#333';
      document.getElementById('iptNome').value = sJ.bname;
  }

  let r = await fetch('/list'); let arr = await r.json();
  let ul=document.getElementById('lista'); ul.innerHTML='';
  arr.forEach(n=>{
    ul.innerHTML+=`<li>${n} <div class="acoes"><button class="btn-play" onclick="play('${n}')">▶</button><a href="#" onclick="del('${n}')">apagar</a></div></li>`;
  });
  
  let sel = sJ.sel;
  let sHtml = '<option value="">(nenhum)</option>';
  for(let i=0; i<sel.f.length; i++) sHtml += `<option value="${sel.f[i]}">${sel.f[i]}</option>`;
  
  document.getElementById('selTrack').innerHTML = sHtml;
  
  for(let i=0; i<numFitas; i++){
    if(document.activeElement.id != 'inTd'+i) document.getElementById('inTd'+i).value = sJ.td[i];
    if(document.activeElement.id != 'inB'+i) document.getElementById('inB'+i).value = sJ.b[i];
    document.getElementById('enT'+i).checked = sJ.en[i];
  }
  
  if(document.activeElement.id != 'selTrack' && document.getElementById('selTrack').options.length > sJ.curTrack + 1) {
    document.getElementById('selTrack').selectedIndex = sJ.curTrack + 1;
  }
  
  let kbUsado = (sJ.used / 1024).toFixed(1);
  let kbTotal = (sJ.total / 1024).toFixed(1);
  let porcentagem = ((sJ.used / sJ.total) * 100).toFixed(0);
  document.getElementById('textMemory').innerText = `${kbUsado} KB usados de ${kbTotal} KB totais (${porcentagem}%)`;
  document.getElementById('barMemory').style.width = porcentagem + '%';
  if (porcentagem > 90) document.getElementById('barMemory').style.background = '#e74c3c';
  else if (porcentagem > 70) document.getElementById('barMemory').style.background = '#f1c40f';
  else document.getElementById('barMemory').style.background = '#2ecc71';
}

async function setTrack(idx_or_name) {
  let tsel = document.getElementById('selTrack');
  await fetch('/setTrack?i=' + tsel.selectedIndex);
}

async function setBase(t, v) {
  await fetch('/setBase?t='+t+'&v='+v);
}

async function setThresh(t, v) {
  await fetch('/setThresh?t='+t+'&v='+v);
  alert('Gatilho ' + t + ' salvo manualmente com o valor ' + v + '!');
}

async function setEn(t, v) { await fetch('/setEn?t='+t+'&v='+(v?1:0)); }

async function getLiveTouch(){
  try {
    let r = await fetch('/touch_status');
    let j = await r.json();
    for(let i=0; i<numFitas; i++){
      document.getElementById('valT'+i).innerText = j.t[i];
      document.getElementById('calT'+i).innerText = `Última calibração -> Máx: ${j.umax[i]} | Mín: ${j.umin[i]}`;
      // Atualiza as caixinhas automaticamente com a nova calibração (só se não estiver digitando nelas)
      if(document.activeElement.id != 'inB'+i && j.b) document.getElementById('inB'+i).value = j.b[i];
      if(document.activeElement.id != 'inTd'+i && j.td) document.getElementById('inTd'+i).value = j.td[i];
    }
    
    let ind = document.getElementById('indicadorVoz');
    if(j.play){ ind.style.background='#2ecc71'; ind.style.color='#000'; ind.innerText='🎵 TOCANDO AGORA!'; }
    else { ind.style.background='#333'; ind.style.color='#888'; ind.innerText='Boi em silêncio'; }
    
    // Sincroniza a caixa de seleção de áudio com a música atual da placa (ignorando o index 0 que é o "(nenhum)")
    if(document.activeElement.id != 'selTrack' && document.getElementById('selTrack').options.length > j.curTrack + 1) {
      document.getElementById('selTrack').selectedIndex = j.curTrack + 1;
    }
  } catch(e){}
}
setInterval(getLiveTouch, 500);

async function calib(){
  let b = document.getElementById('btnCalib');
  b.innerText = 'Aguarde 10s (NÃO TOQUE NA FITA!)...';
  b.style.background = '#e74c3c';
  await fetch('/calib');
  setTimeout(() => {
    b.innerText = 'Calibrar Sensibilidade';
    b.style.background = '#f39c12';
    alert('Calibração Perfeita (10s) Concluída!');
  }, 10500);
}

async function zerar(){
  await fetch('/zerar');
  alert('Contador de segurança zerado!');
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
  
  // Tenta abrir no LittleFS, se falhar, tenta PROGMEM
  file = new AudioFileSourceLittleFS(("/" + nomeArquivo).c_str());
  if(!file->isOpen()){
    delete file;
    file = new AudioFileSourcePROGMEM(progmem_audio_data[0], progmem_audio_sizes[0]);
  }
  
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
    ligarRele();
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
    desligarRele();
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

String carregarFaixasDaMemoria() {
  String json = "{\"f\":[";
  bool first = true;
  playlistSize = 0;
  
  // Adiciona áudios nativos (PROGMEM)
  for(int i = 0; i < num_progmem_audios && playlistSize < MAX_PLAYLIST; i++) {
    String name = String(progmem_audio_names[i]);
    playlist[playlistSize] = name;
    playlistSize++;
    
    if(!first) json += ",";
    json += "\"" + name + "\"";
    first = false;
  }
  
  // Adiciona arquivos do LittleFS
  File root = LittleFS.open("/");
  File f = root.openNextFile();
  while(f && playlistSize < MAX_PLAYLIST){
    if(!f.isDirectory()){
      String name = f.name();
      if(name.endsWith(".mp3") && name != "desconectado.mp3"){
        playlist[playlistSize] = name;
        playlistSize++;
        
        if(!first) json += ",";
        json += "\"" + name + "\"";
        first = false;
      }
    }
    f = root.openNextFile();
  }
  json += "]}";
  return json;
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

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r){
    String json = "{\"vol\":" + String(vol) + ",\"lic\":" + (licenciado?"true":"false") + ",\"sel\":";
    json += carregarFaixasDaMemoria();
    json += ",\"curTrack\":" + String(currentTrackIndex) + ",\"bname\":\"" + bname + "\",\"td\":[";
    for(int i=0; i<NUM_TOUCH; i++) { json += String(threshDown[i]); if(i < NUM_TOUCH-1) json += ","; }
    json += "],\"b\":[";
    for(int i=0; i<NUM_TOUCH; i++) { json += String(baseTouch[i]); if(i < NUM_TOUCH-1) json += ","; }
    json += "],\"en\":[";
    for(int i=0; i<NUM_TOUCH; i++) { json += (touchHab[i] ? "true" : "false"); if(i < NUM_TOUCH-1) json += ","; }
    json += "],\"used\":" + String(LittleFS.usedBytes()) + ",\"total\":" + String(LittleFS.totalBytes()) + "}";
    r->send(200, "application/json", json);
  });

  server.on("/setTrack", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("i")){
      int i = r->getParam("i")->value().toInt();
      // O índex 0 no HTML geralmente é a opção "(nenhum)"
      if (i > 0 && i - 1 < playlistSize) {
        currentTrackIndex = i - 1;
        prefs.putInt("curTrk", currentTrackIndex);
      }
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *r){
    String file = r->getParam("file")->value();
    LittleFS.remove("/" + file);
    r->send(200, "text/plain", "ok");
  });

  server.on("/volume", HTTP_GET, [](AsyncWebServerRequest *r){
    int v = r->getParam("v")->value().toInt();
    vol = v / 100.0;
    out->SetGain(vol);
    r->send(200, "text/plain", "ok");
  });
  
  server.on("/play", HTTP_GET, [](AsyncWebServerRequest *r){
    String file = r->getParam("file")->value();
    ligarAmpliETocar(file);
    r->send(200, "text/plain", "ok");
  });

  server.on("/calib", HTTP_GET, [](AsyncWebServerRequest *r){
    calibrando = true;
    tempoFimCalibracao = millis() + 10000;
    for(int i=0; i<NUM_TOUCH; i++) {
      somaLeituraT[i] = 99999; // Vamos usar essa variável temporariamente para guardar o valor MÍNIMO (iniciando bem alto)
      contLeituraT[i] = 0;
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/zerar", HTTP_GET, [](AsyncWebServerRequest *r){
    offline_plays = 0;
    prefs.putInt("offp", 0);
    r->send(200, "text/plain", "zerado");
  });

  server.on("/setThresh", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("t") && r->hasParam("v")){
      int t = r->getParam("t")->value().toInt();
      int v = r->getParam("v")->value().toInt();
      if(t >= 0 && t < NUM_TOUCH) { threshDown[t] = v; prefs.putInt(("td"+String(t)).c_str(), v); }
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/setBase", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("t") && r->hasParam("v")){
      int t = r->getParam("t")->value().toInt();
      int v = r->getParam("v")->value().toInt();
      if(t >= 0 && t < NUM_TOUCH) { baseTouch[t] = v; prefs.putInt(("bT"+String(t)).c_str(), v); }
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/setEn", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("t") && r->hasParam("v")){
      int t = r->getParam("t")->value().toInt();
      int v = r->getParam("v")->value().toInt();
      if(t >= 0 && t < NUM_TOUCH) { touchHab[t] = (v == 1); prefs.putBool(("en"+String(t)).c_str(), touchHab[t]); }
    }
    r->send(200, "text/plain", "OK");
  });

  server.on("/touch_status", HTTP_GET, [](AsyncWebServerRequest *r){
    String json = "{";
    json += "\"t\":["; for(int i=0; i<NUM_TOUCH; i++) { json += String(valT[i]); if(i < NUM_TOUCH-1) json += ","; } json += "],";
    json += "\"b\":["; for(int i=0; i<NUM_TOUCH; i++) { json += String(baseTouch[i]); if(i < NUM_TOUCH-1) json += ","; } json += "],";
    json += "\"td\":["; for(int i=0; i<NUM_TOUCH; i++) { json += String(threshDown[i]); if(i < NUM_TOUCH-1) json += ","; } json += "],";
    json += "\"umin\":["; for(int i=0; i<NUM_TOUCH; i++) { json += String(ultimoMin[i]); if(i < NUM_TOUCH-1) json += ","; } json += "],";
    json += "\"umax\":["; for(int i=0; i<NUM_TOUCH; i++) { json += String(ultimoMax[i]); if(i < NUM_TOUCH-1) json += ","; } json += "],";
    json += "\"curTrack\":" + String(currentTrackIndex) + ",";
    json += "\"used\":" + String(LittleFS.usedBytes()) + ",\"total\":" + String(LittleFS.totalBytes()) + ",";
    json += "\"play\":"; json += (mp3->isRunning() ? "true" : "false");
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

TaskHandle_t telegramTaskHandle;
void telegramTask(void *pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && !mp3->isRunning()) {
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      while (numNewMessages) {
        handleNewMessages(numNewMessages);
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }
    }
    vTaskDelay(60000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  desligarRele();
  pinMode(BOTAO1_PIN, INPUT_PULLUP);
  pinMode(BOTAO2_PIN, INPUT_PULLUP);

  if(!LittleFS.begin(true)){
    Serial.println("Erro ao montar LittleFS");
    return;
  }

  out = new AudioOutputCustomDAC(25); // Usa o Pino 25 para o DAC nativo
  
  mp3 = new AudioGeneratorMP3();

  prefs.begin("audio", false);
  for(int i=0; i<NUM_TOUCH; i++){
    valT[i] = 2000;
    baseTouch[i] = prefs.getInt(("bT"+String(i)).c_str(), 2000);
    threshDown[i] = prefs.getInt(("td"+String(i)).c_str(), 1900);
    touchHab[i] = prefs.getBool(("en"+String(i)).c_str(), (i < 2)); // Default: só a Fita 1 e 2 ligadas
    ultimoTouch[i] = 0;
    toquesValidos[i] = 0;
    aguardandoSoltar[i] = false;
    ultimoMin[i] = 0;
    ultimoMax[i] = 0;
  }
  
  licenciado = prefs.getBool("lic", false);
  bname = prefs.getString("bname", "Boi");
  
  carregarFaixasDaMemoria(); // Popula o Array da Playlist inicialmente
  currentTrackIndex = prefs.getInt("curTrk", 0);
  if (currentTrackIndex >= playlistSize && playlistSize > 0) currentTrackIndex = 0;
  offline_plays = prefs.getInt("offp", 0);
  
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
    } else {
      WiFi.disconnect(); // Corta a tentativa fantasma no fundo para não travar a criação do AP
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
  
  xTaskCreatePinnedToCore(telegramTask, "Telegram", 8192, NULL, 1, &telegramTaskHandle, 0);
}

void loop() {
  if (calibrando) {
    if (millis() < tempoFimCalibracao) {
      for(int i=0; i<NUM_TOUCH; i++){
        if(!touchHab[i]) continue;
        
        // Aloca array dinâmico só durante a calibração
        if(!calibData[i]) {
          calibData[i] = new uint16_t[500];
          calibCount[i] = 0;
        }
        
        int val = touchRead(TOUCH_PINS[i]);
        if(val > 0 && calibCount[i] < 500) {
          calibData[i][calibCount[i]] = val;
          calibCount[i]++;
        }
      }
      delay(20); // 20ms * 500 leituras = 10 segundos!
    } else {
      calibrando = false;
      for(int i=0; i<NUM_TOUCH; i++){
        if(!touchHab[i] || !calibData[i] || calibCount[i] == 0) continue;
        
        // Ordenação Bubble Sort Simples para achar o verdadeiro mínimo estável
        for (int a = 0; a < calibCount[i] - 1; a++) {
          for (int b = 0; b < calibCount[i] - a - 1; b++) {
            if (calibData[i][b] > calibData[i][b+1]) {
              uint16_t temp = calibData[i][b];
              calibData[i][b] = calibData[i][b+1];
              calibData[i][b+1] = temp;
            }
          }
        }
        
        // Descarta os primeiros ~2% (falhas e interferências de 1 milissegundo)
        // Se gravou 500 números, pega o do índice 10 (ignora os 10 menores fantasmas)
        int indexSeguro = (calibCount[i] > 20) ? (calibCount[i] / 50) : 0;
        long baseSegura = calibData[i][indexSeguro];
        
        ultimoMax[i] = calibData[i][calibCount[i]-1]; 
        ultimoMin[i] = baseSegura;
        
        // A Base vira O MENOR NÚMERO DA PESQUISA PURIFICADO
        baseTouch[i] = baseSegura; 
        
        // O Gatilho vira automaticamente a Base menos 50, conforme pedido
        threshDown[i] = baseTouch[i] - 50;
        
        prefs.putInt(("bT"+String(i)).c_str(), baseTouch[i]);
        prefs.putInt(("td"+String(i)).c_str(), threshDown[i]);
        
        delete[] calibData[i];
        calibData[i] = nullptr;
      }
    }
  } // Fim de calibrando sem dar return

  if (mp3->isRunning()) { 
    if (!mp3->loop()) mp3->stop(); 
  }

  if (esperandoAmpli && (millis() - tempoInicioAmpli >= 2000)) {
    esperandoAmpli = false;
    tocarAgora(trackPendente);
    trackPendente = "";
  }

  // Leitura dos sensores a cada 20ms (RESPOSTA INSTANTÂNEA E FILTRADA)
  if (millis() - lastSensoresTime > 20) {
    lastSensoresTime = millis();
    
    // Lê o pino cru na hora, com filtro ultra-leve para ignorar pulos e zeros absolutos
    for(int i=0; i<NUM_TOUCH; i++){
      if(touchHab[i]) {
        int leituraRaw = touchRead(TOUCH_PINS[i]);
        if (leituraRaw > 0) { // Ignora falhas de leitura (0) do hardware
          // Suaviza o ruído em apenas 2 passos (super rápido, não causa lag)
          valT[i] = (valT[i] + leituraRaw) / 2;
        }
      }
    }

    // Só permite acionamento por sensor se estiver licenciado, destravado e NÃO ESTIVER CALIBRANDO
    if (licenciado && !bloqueado && !calibrando) {
      if (digitalRead(PIR_PIN) == HIGH) {
        if (!ampliLigado) {
           ultimaAtividade = millis();
           ligarRele();
           ampliLigado = true;
        }
      }

      for(int i=0; i<NUM_TOUCH; i++){
        if(!touchHab[i]) continue;
        if (valT[i] < threshDown[i]) {
          if (!aguardandoSoltar[i] && !mp3->isRunning()) {
            toquesValidos[i]++;
            // Exige apenas 2 toques válidos (40ms de dedo encostado) para tocar NA HORA
            if (toquesValidos[i] >= 2 && millis() - ultimoTouch[i] > DEBOUNCE_MS) {
              ultimoTouch[i] = millis();
              toquesValidos[i] = 0;
              aguardandoSoltar[i] = true; // OBRIGA A SOLTAR O FIO
              if (playlistSize > 0) { ligarAmpliETocar(playlist[currentTrackIndex]); }
            }
          }
        } else {
          toquesValidos[i] = 0;
          aguardandoSoltar[i] = false; // Liberado para tocar de novo
        }
      }

      if (digitalRead(BOTAO1_PIN) == LOW && millis() - ultimoBotao1 > DEBOUNCE_MS && !mp3->isRunning()) {
        ultimoBotao1 = millis();
        if (playlistSize > 0) { ligarAmpliETocar(playlist[currentTrackIndex]); }
      }

      // Botão 2 (Avançar Faixa) - Funciona mesmo se já estiver tocando música!
      if (digitalRead(BOTAO2_PIN) == LOW && millis() - ultimoBotao2 > DEBOUNCE_MS) {
        ultimoBotao2 = millis();
        if (playlistSize > 0) { 
          currentTrackIndex = (currentTrackIndex + 1) % playlistSize;
          prefs.putInt("curTrk", currentTrackIndex);
          ligarAmpliETocar(playlist[currentTrackIndex]); // Já toca a próxima para ouvir
        }
      }
    }
  }

  desligarAmpliSeOcioso();
}

