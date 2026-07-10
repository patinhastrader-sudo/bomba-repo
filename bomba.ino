#define BLYNK_TEMPLATE_ID "TMPL2QGXSK92k"
#define BLYNK_TEMPLATE_NAME "Bomba Chácara V48"
#define BLYNK_AUTH_TOKEN "1yGo7t1318rg_B-ZRo5y8ofRORUrRyKu"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BlynkSimpleEsp32.h> 
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoOTA.h>
#include <Update.h>         // Biblioteca necessária para gravar o arquivo do GitHub
#include <esp_task_wdt.h>
#include <Preferences.h> 
#include "time.h"

// --- CONFIGURAÇÕES DE REDE E TELEGRAM ---
const char* ssid = "LUISCARLOS_Ext";
const char* password = "rolujuda";
#define BOT_TOKEN_BOMBA "8462962534:AAE2SukSynJbCq4rNFJ9Czq5s6r5DNa0sAc"
#define CHAT_ID "-1003732320316"
#define SEU_ID_PESSOAL "361084988"

// 🌐 LINK DA SUA NUVEM NO GITHUB
// Substitua o link abaixo pelo link "RAW" do seu arquivo bomba.bin no GitHub (Passo 4 que fizemos antes)
const String urlBinarioNuvem = "https://raw.githubusercontent.com/seu-usuario/bomba-repo/main/bomba.bin";

// --- MAPEAMENTO DE HARDWARE REAL ---
#define SS 5
#define RST 14
#define DIO0 22 
#define RELE_LIGAR 26  
#define RELE_FREIO 27  
#define CHAVE_LIGAR 4   
#define CHAVE_MODO 13  

Preferences memInterna;
WiFiClientSecure cMsg; HTTPClient hMsg;
WiFiClientSecure cLer; HTTPClient hLer;
WiFiClientSecure cCloud; 

// --- VARIÁVEIS DE ESTADO ---
bool estadoBomba = false;
bool manualPeloApp = false;
bool bootEnviado = false; 
bool foiReinicioRotina = false; 
String statusAgua = "AGUARDANDO... 📡";
String memoriaEstadoCaixa = "DESCONHECIDO"; 

unsigned long configTempoMinutos = 35; 
unsigned long fimTemporizador = 0;
unsigned long ultimoSinalLoRa = 0;
unsigned long ultimaVezComunicouTelegram = 0;
unsigned long pacotesRecebidosLoRa = 0;
long lastUpdateId = 0;

unsigned long tempoUltimoReinicio4h = 0;
static bool botaoLigarEstadoAnterior = false;
static unsigned long tempoUltimoDebounceBotao = 0;

// 🎛️ TECLADO PADRÃO OURO COMPLETO (Incluído o botão /atualizar para você usar no celular)
const String kbBomba = "{\"keyboard\":[[\"/ligar\", \"/desligar\"],[\"/status\", \"/reiniciar\"],[\"/auto\", \"/manual\"],[\"/tempo 30min\", \"/tempo 35min\"],[\"/atualizar\"]],\"resize_keyboard\":true}";

String urlEncode(String str) {
  String out = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c)) out += c;
    else { char buf[4]; sprintf(buf, "%%%02X", (unsigned char)c); out += buf; }
  }
  return out;
}

String traduzirSinal(int rssi) {
  if (WiFi.status() != WL_CONNECTED) return "DESCONECTADO ❌";
  if (rssi >= -60) return "Excelente ⭐";
  if (rssi >= -70) return "Bom/Forte 📶";
  if (rssi >= -80) return "Médio 🆗";
  return "Fraco ⚠️";
}

String dataHora() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "📅 [Sincronizando...]";
  char buffer[50];
  strftime(buffer, 50, "📅 %d/%m/%Y | 🕒 %H:%M:%S", &timeinfo);
  return String(buffer);
}

void atualizarDadosNoBlynk() {
  if (Blynk.connected()) {
    Blynk.virtualWrite(V1, estadoBomba ? 1 : 0); 
    Blynk.virtualWrite(V2, statusAgua);         
    int porcentagem = 50;
    if (memoriaEstadoCaixa == "CHEIO") porcentagem = 100;
    else if (memoriaEstadoCaixa == "VAZIO") porcentagem = 0;
    Blynk.virtualWrite(V3, porcentagem);        
    Blynk.virtualWrite(V4, manualPeloApp ? 1 : 0);
    Blynk.virtualWrite(V5, WiFi.RSSI());
  }
}

void enviarMensagem(String msg) {
  if (WiFi.status() != WL_CONNECTED) return;
  esp_task_wdt_reset(); 
  cMsg.setInsecure(); hMsg.setTimeout(4000); 
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN_BOMBA) + "/sendMessage?chat_id=" + String(CHAT_ID) + "&parse_mode=Markdown&text=" + urlEncode(msg) + "&reply_markup=" + urlEncode(kbBomba);
  hMsg.begin(cMsg, url); if (hMsg.GET() == 200) { ultimaVezComunicouTelegram = millis(); } hMsg.end();
}

void enviarStatusOuro(String titulo) {
  String s = "📌 *" + titulo + " (V49.16)*\n" + dataHora() + "\n";
  s += "------------------------------------------\n";
  s += "🔋 *ESTADO DA BOMBA:* " + String(estadoBomba ? "LIGADA 💧" : "DESLIGADA 🛑") + "\n";
  bool modoManual = (manualPeloApp || digitalRead(CHAVE_MODO) == LOW);
  s += "⚙️ *MODO DE OPERAÇÃO:* " + String(modoManual ? "MANUAL ⚠️" : "AUTOMÁTICO 🤖") + "\n";
  s += "⏱️ *TIMER PADRÃO:* " + String(configTempoMinutos) + " min\n";
  if (estadoBomba) {
    unsigned long restante = (fimTemporizador > millis()) ? (fimTemporizador - millis()) / 60000 : 0;
    s += "⏳ *TEMPO RESTANTE:* " + String(restante) + " min\n";
  }
  s += "------------------------------------------\n";
  String alertaLoRa = (millis() - ultimoSinalLoRa > 600000 && ultimoSinalLoRa != 0) ? " (SINAL ANTIGO ⚠️)" : "";
  s += "💧 *CAIXA:* " + statusAgua + alertaLoRa + "\n";
  s += "📡 *RÁDIO LoRa:* OK ✅ (Pacotes: " + String(pacotesRecebidosLoRa) + ")\n";
  s += "📶 *SINAL WIFI:* " + traduzirSinal(WiFi.RSSI()) + "\n";
  s += "⏱️ *SISTEMA ATIVO:* " + String(millis() / 3600000) + " horas";

  enviarMensagem(s);
  atualizarDadosNoBlynk();
}

void hwLigar(String origem) {
  if (estadoBomba) return;
  bool modoManual = (manualPeloApp || digitalRead(CHAVE_MODO) == LOW);
  if (!modoManual && memoriaEstadoCaixa == "CHEIO") {
    enviarMensagem("⚠️ *BLOQUEIO* via " + origem + " rejeitado: Caixa CHEIA!");
    return;
  }
  digitalWrite(RELE_FREIO, HIGH); delay(150);
  digitalWrite(RELE_LIGAR, LOW);  estadoBomba = true;
  fimTemporizador = millis() + (configTempoMinutos * 60000);
  
  memInterna.begin("bomba_cfg", false);
  memInterna.putBool("bombaLigada", true);
  memInterna.end();

  enviarStatusOuro("BOMBA LIGADA VIA " + origem);
}

void hwDesligar(String origem) {
  if (!estadoBomba) return; 
  
  digitalWrite(RELE_LIGAR, HIGH); delay(100);
  digitalWrite(RELE_FREIO, LOW);  estadoBomba = false; fimTemporizador = 0;
  
  memInterna.begin("bomba_cfg", false);
  memInterna.putBool("bombaLigada", false);
  memInterna.end();

  enviarStatusOuro("BOMBA DESLIGADA VIA " + origem);
}

// 🌐 FUNÇÃO EXCLUSIVA QUE BUSCA O ARQUIVO NO GITHUB REMOTAMENTE
void executarAtualizacaoNuvem() {
  if (WiFi.status() != WL_CONNECTED) {
    enviarMensagem("❌ Falha: Sem Wi-Fi para buscar atualização na nuvem.");
    return;
  }
  if (estadoBomba) {
    enviarMensagem("❌ *ERRO:* Não é seguro atualizar com a bomba ligada!");
    return;
  }

  enviarMensagem("📥 Conectando ao GitHub... Buscando arquivo `bomba.bin`...");
  
  cCloud.setInsecure(); 
  HTTPClient http;
  http.begin(cCloud, urlBinarioNuvem);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    if (contentLength <= 0) {
      enviarMensagem("❌ Erro: Tamanho do arquivo inválido na nuvem.");
      http.end();
      return;
    }

    bool canBegin = Update.begin(contentLength);
    if (canBegin) {
      enviarMensagem("⚡ Arquivo localizado! Gravando na memória flash... Aguarde.");
      WiFiClient* client = http.getStreamPtr();
      size_t written = Update.writeStream(*client);

      if (written == contentLength) {
        enviarMensagem("✅ Gravação concluída com sucesso!");
      } else {
        enviarMensagem("❌ Erro na gravação.");
      }

      if (Update.end()) {
        if (Update.isFinished()) {
          enviarMensagem("🔄 Reprogramação concluída! Reiniciando o sistema...");
          delay(2000);
          ESP.restart();
        }
      } else {
        enviarMensagem("❌ Erro no fechamento do arquivo.");
      }
    } else {
      enviarMensagem("❌ Espaço insuficiente na memória flash.");
    }
  } else {
    enviarMensagem("❌ Falha ao acessar o link. Verifique se o arquivo está público no GitHub.");
  }
  http.end();
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V4); 
  Blynk.syncVirtual(V6); 
}

BLYNK_WRITE(V1) {
  int acaoBotao = param.asInt();
  if (acaoBotao == 1 && !estadoBomba) hwLigar("APP BLYNK (BOTÃO)");
  else if (acaoBotao == 0 && estadoBomba) hwDesligar("APP BLYNK (BOTÃO)");
}

BLYNK_WRITE(V4) {
  manualPeloApp = (param.asInt() == 1);
  atualizarDadosNoBlynk();
}

BLYNK_WRITE(V6) {
  int sinalCaixaInternet = param.asInt();
  String estadoAnteriorMemoria = memoriaEstadoCaixa;

  if (sinalCaixaInternet == 0) { statusAgua = "CHEIA ✅"; memoriaEstadoCaixa = "CHEIO"; } 
  else if (sinalCaixaInternet == 1) { statusAgua = "VAZIA 💧"; memoriaEstadoCaixa = "VAZIO"; } 
  else if (sinalCaixaInternet == 2) { statusAgua = "PELA METADE 🟡"; memoriaEstadoCaixa = "MEIO"; }

  if (!(manualPeloApp || digitalRead(CHAVE_MODO) == LOW)) {
    if (memoriaEstadoCaixa == "VAZIO" && !estadoBomba) hwLigar("AUTOMAÇÃO INTERNET (BLYNK FALLBACK)");
    else if (memoriaEstadoCaixa == "CHEIO" && estadoBomba) hwDesligar("AUTOMAÇÃO INTERNET (BLYNK FALLBACK)");
  }
  if (memoriaEstadoCaixa != estadoAnteriorMemoria) atualizarDadosNoBlynk();
}

void lerTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;
  esp_task_wdt_reset(); 
  cLer.setInsecure(); hLer.setTimeout(4000); 
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN_BOMBA) + "/getUpdates?offset=" + String(lastUpdateId + 1) + "&timeout=1";
  
  hLer.begin(cLer, url);
  if (hLer.GET() == 200) {
    String res = hLer.getString(); int idx = 0;
    while ((idx = res.indexOf("\"update_id\":", idx)) != -1) {
      idx += 12; int endIdx = res.indexOf(",", idx);
      if (endIdx != -1) { long updateId = res.substring(idx, endIdx).toInt(); if (updateId > lastUpdateId) lastUpdateId = updateId; }
      
      if (res.indexOf("\"text\":\"/ligar\"", idx) != -1) hwLigar("APP TELEGRAM");
      else if (res.indexOf("\"text\":\"/desligar\"", idx) != -1) hwDesligar("APP TELEGRAM");
      else if (res.indexOf("\"text\":\"/status\"", idx) != -1) enviarStatusOuro("RELATÓRIO OPERACIONAL");
      else if (res.indexOf("\"text\":\"/auto\"", idx) != -1) { manualPeloApp = false; enviarStatusOuro("MODO AUTOMÁTICO VIA APP"); }
      else if (res.indexOf("\"text\":\"/manual\"", idx) != -1) { manualPeloApp = true; enviarStatusOuro("MODO MANUAL VIA APP"); }
      else if (res.indexOf("\"text\":\"/atualizar\"", idx) != -1) { executarAtualizacaoNuvem(); } // 💥 CHAMA A ATUALIZAÇÃO DA NUVEM
      else if (res.indexOf("\"text\":\"/reiniciar\"", idx) != -1) {
        if(estadoBomba) {
          enviarMensagem("❌ *REINÍCIO NEGADO:* A bomba está em funcionamento e não pode ser resetada agora!");
        } else {
          enviarMensagem("🔄 Executando comando de reinicialização remota..."); delay(1500); ESP.restart();
        }
      }
      else if (res.indexOf("\"text\":\"/tempo 30min\"", idx) != -1) { configTempoMinutos = 30; enviarMensagem("⏱️ Limite definido para *30 minutos*."); }
      else if (res.indexOf("\"text\":\"/tempo 35min\"", idx) != -1) { configTempoMinutos = 35; enviarMensagem("⏱️ Limite definido para o padrão de *35 minutos*."); }
      if (endIdx != -1) idx = endIdx; else idx++;
    }
  }
  hLer.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(RELE_LIGAR, OUTPUT); digitalWrite(RELE_LIGAR, HIGH);
  pinMode(RELE_FREIO, OUTPUT); digitalWrite(RELE_FREIO, LOW);
  pinMode(CHAVE_LIGAR, INPUT_PULLUP); pinMode(CHAVE_MODO, INPUT_PULLUP);

  WiFi.begin(ssid, password);
  configTime(-10800, 0, "pool.ntp.org");
  Blynk.config(BLYNK_AUTH_TOKEN);

  LoRa.setPins(SS, RST, DIO0);
  if (LoRa.begin(433E6)) {
    LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
    LoRa.setSpreadingFactor(12); LoRa.setSignalBandwidth(125E3);
  }

  ArduinoOTA.begin(); 
  tempoUltimoReinicio4h = millis(); ultimaVezComunicouTelegram = millis();

  int t = 0; while (WiFi.status() != WL_CONNECTED && t < 10) { delay(500); t++; }
  if(WiFi.status() == WL_CONNECTED) { 
    Blynk.connect(); 
    hLer.begin(cLer, "https://api.telegram.org/bot" + String(BOT_TOKEN_BOMBA) + "/getUpdates?offset=-1");
    if(hLer.GET() == 200) {
      String res = hLer.getString(); int idx = res.indexOf("\"update_id\":");
      if(idx > 0) { lastUpdateId = res.substring(idx + 12, res.indexOf(",", idx)).toInt(); }
    }
    hLer.end();
  }

  memInterna.begin("bomba_cfg", false);
  foiReinicioRotina = memInterna.getBool("reinicioRotina", false);
  memInterna.putBool("reinicioRotina", false); 
  memInterna.putBool("bombaLigada", false); 
  memInterna.end();

  esp_task_wdt_config_t twdt_config = { .timeout_ms = 30000, .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, .trigger_panic = true };
  esp_task_wdt_reconfigure(&twdt_config); esp_task_wdt_add(NULL); 
}

void loop() {
  esp_task_wdt_reset(); ArduinoOTA.handle();   

  if (WiFi.status() == WL_CONNECTED) {
    Blynk.run();
    
    if (!bootEnviado) { 
      if(foiReinicioRotina) {
        enviarStatusOuro("Reinicio de rotina executado");
      } else {
        enviarStatusOuro("Sistema online novamente");
      }
      bootEnviado = true; 
    }
  }

  if (WiFi.status() != WL_CONNECTED && millis() % 15000 < 20) { WiFi.begin(ssid, password); }

  static unsigned long tempoUltimaLeituraTelegram = 0;
  if (millis() - tempoUltimaLeituraTelegram > 4000) { lerTelegram(); tempoUltimaLeituraTelegram = millis(); }

  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String m = ""; while (LoRa.available()) m += (char)LoRa.read();
    ultimoSinalLoRa = millis(); pacotesRecebidosLoRa++;
    String estadoAnteriorMemoria = memoriaEstadoCaixa;
    
    if (m == "CHEIO") { statusAgua = "CHEIA ✅ (LoRa)"; memoriaEstadoCaixa = "CHEIO"; } 
    else if (m == "VAZIO") { statusAgua = "VAZIA 💧 (LoRa)"; memoriaEstadoCaixa = "VAZIO"; } 
    else if (m == "MEIO") { statusAgua = "PELA METADE 🟡 (LoRa)"; memoriaEstadoCaixa = "MEIO"; } 
    
    if (!(manualPeloApp || digitalRead(CHAVE_MODO) == LOW)) {
      if (memoriaEstadoCaixa == "VAZIO" && !estadoBomba) hwLigar("AUTOMAÇÃO COMPLETA (LORA)");
      if (memoriaEstadoCaixa == "CHEIO" && estadoBomba) hwDesligar("AUTOMAÇÃO COMPLETA (LORA)");
    }
    if (memoriaEstadoCaixa != estadoAnteriorMemoria) { atualizarDadosNoBlynk(); }
  }

  if (estadoBomba && millis() > fimTemporizador) { hwDesligar("TIMER DE SEGURANÇA INTERNO"); }

  bool botaoLigarPremido = (digitalRead(CHAVE_LIGAR) == LOW);
  if (botaoLigarPremido && !botaoLigarEstadoAnterior) {
    if (millis() - tempoUltimoDebounceBotao > 400) {
      if (!estadoBomba) hwLigar("BOTÃO FÍSICO DO PAINEL"); else hwDesligar("BOTÃO FÍSICO DO PAINEL");
      tempoUltimoDebounceBotao = millis();
    }
  }
  botaoLigarEstadoAnterior = botaoLigarPremido;

  static unsigned long ultimoEnvioBlynk = 0;
  if (millis() - ultimoEnvioBlynk > 300000) { atualizarDadosNoBlynk(); ultimoEnvioBlynk = millis(); }

  if (millis() - tempoUltimoReinicio4h > 14400000) { 
    if(!estadoBomba) {
      memInterna.begin("bomba_cfg", false);
      memInterna.putBool("reinicioRotina", true);
      memInterna.end();
      delay(500);
      ESP.restart();
    } else {
      tempoUltimoReinicio4h = millis() - 1200000; 
    }
  }
  if (millis() - ultimaVezComunicouTelegram > 480000) { 
    if(!estadoBomba) { ESP.restart(); } 
  }
}
