// 修改日期：2017-06-04
// ************************************************************************
// S3X_Proxy：提供多個 Client 連線
// ************************************************************************
// 零件清單：ESP01S、TTL轉RS485硬體自動流向控制模組、LED*2、R220電阻*2、
//           AMS1117 3.3V 電源降壓模組、Micro USB 母座 PCB 轉接板
// 設定建議：ESP01S 請選用 【Flash Size 1M(64K SPIFFS)】避免 OTA 上傳有問題
//           ESP8266的快閃記憶體結構 https://swf.com.tw/?p=905
// 注意事項：利用 OTA 上傳有時會卡住造成 UART 瘋狂亂送訊息造成 主機(P31) 收
//           到錯誤資訊造成水溫設定錯亂
// ************************************************************************
// http://arduino.tw/allarticlesindex/2009-09-06-18-37-08/169-arduinohd.html
#include <EEPROM.h> 
//-------------------------------------------------------
// https://github.com/JChristensen/Timer
// http://playground.arduino.cc/Code/Timer 
// http://yehnan.blogspot.tw/2012/03/arduino.html
#include "Timer.h"      
//-------------------------------------------------------
#include "Blink.h"
#include "S3X.h"
//-------------------------------------------------------
#include <ESP8266WiFi.h>
#include <Ticker.h>
//-------------------------------------------------------
// http://esp8266.github.io/Arduino/versions/2.0.0/doc/ota_updates/ota_updates.html#classic-ota-configuration
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
//-------------------------------------------------------
#include <ESP8266WebServer.h>
//-------------------------------------------------------
#include "ThingSpeak.h"
// ************************************************************************
String RS485_ConfigText[3] = {"8-N-1","8-E-1","8-O-1"};   // RS-485 組態文字
                                                          // RS-485 組態清單
SerialConfig RS485_Config[3] = {SERIAL_8N1, SERIAL_8E1, SERIAL_8O1}; 
#define RS485_Speed   9600                                // RS-485 Baud Rate
#define RS485_Port    Serial                               // RS-485 使用的 UART Port
//-------------------------------------------------------
struct StoreStruct {
  byte Head;                                              // 資料開頭：0~255 亂數
  byte S3X_IdentIndex;                                    // S3X 設備識別索引
  byte S3X_RunModeIndex;                                  // S3X 運行模式
  byte RS485_ConfigIndex;                                 // RS-485 組態值索引
  byte Disable_AP;                                        // 停用 Soft-AP
  char WiFi_SSID[16+1];                                   // WiFi SSID 名稱
  char WiFi_PASS[16+1];                                   // WiFi 連線密碼
  byte ThingSpeak_Upload_MinuteIndex;                     // ThingSpeak 資料上傳間隔索引
  unsigned long ThingSpeak_ChannelNumber;                 // ThingSpeak Channer Number
  char ThingSpeak_WriteAPIKey[16+1];                      // ThingSpeak API Key
  byte Tail;                                              // 資料結尾：資料開頭的反向值
} myConfig;
//-------------------------------------------------------
String SlaveName[]  = {"S31", "S32", "S33"};              // 設備名稱
//-------------------------------------------------------
const byte Pin_Link_LED            = 2;                   // GPIO 腳位：Link 燈
const byte Pin_WiFi_LED            = 0;                   // GPIO 腳位：WiFi 燈
//-------------------------------------------------------
Timer Timer_SysCheck;                                     // 計時器：系統模式：接收資料顯示、未連線閃爍...
const unsigned long SysCheck_Interval = 500;              // 間隔： 0.5 秒
//-------------------------------------------------------
S3X myS3X;                                                // S3X 類別
//-------------------------------------------------------
Blink Link_Blink;                                         // 閃爍：Link 閃爍
//-------------------------------------------------------
Ticker WiFi_LED_Ticker;                                   // Ticker：WiFi 燈
//-------------------------------------------------------
Timer Timer_ThingSpeak;                                   // 計時器：ThingSpeak 資料上傳...
const unsigned long Time_Minute = 1 * 60 * 1000;          // 間隔：一分鐘
byte Minute_Interval[3] = { 5, 10, 15 };                  // 間隔：分鐘列表
//-------------------------------------------------------
int status = WL_IDLE_STATUS;
WiFiClient WiFi_Client;
//-------------------------------------------------------
char AP_SSID[32+1];                                       // Soft AP 的 SSID
byte WiFi_MAC[6];                                         // WiFi 的 MAC 資料
//-------------------------------------------------------
ESP8266WebServer WebServer(80);                          // Web Server
//-------------------------------------------------------
const char* mDNS_Name = "S3X-Proxy";                      // mDNS 名稱
// ************************************************************************
void setup() {
  //-------------------------------------------------------
  IO_Init();                                              // IO 設置
  //-------------------------------------------------------
  LED_Blink(Pin_WiFi_LED,500,5);
  LED_Blink(Pin_WiFi_LED,100,25);
  //-------------------------------------------------------
  LoadConfig();                                           // 載入設定
  //-------------------------------------------------------
  WiFi_init();                                            // WiFi 設置
  //-------------------------------------------------------
  OTA_Init();                                             // OTA 設置
  //-------------------------------------------------------
  WebServer_Init();                                       // Web Server 設置
  //-------------------------------------------------------
  ThingSpeak.begin(WiFi_Client);                          // ThingSpeak 開始執行
  //-------------------------------------------------------
  UART_Init();                                            // UART 設置
  myS3X.begin(&RS485_Port, RS485_Speed);                  // S3X 開始執行
  //-------------------------------------------------------
  Timer_SysCheck.every(SysCheck_Interval,SystemCheck);    // 計時器：系統檢查
  //-------------------------------------------------------
                                                          // 計時器：ThingSpeak 資料上傳
  Timer_ThingSpeak.every(Time_Minute * Minute_Interval[myConfig.ThingSpeak_Upload_MinuteIndex],ThingSpeak_Upload); 
  //-------------------------------------------------------
}
// ************************************************************************
void loop() {
  //-------------------------------------------------------
  ArduinoOTA.handle();                                   // OTA
  //-------------------------------------------------------
  WebServer_Run();                                        // Web Server 執行
  //-------------------------------------------------------
  myS3X.Run();                                            // S3X 執行
  if (myS3X.PacketFinish()==true) {
    Timer_ThingSpeak.update();                            // 計時器：ThingSpeak 資料上傳
    Link_Blink.Active();
  }
  //-------------------------------------------------------
  Timer_SysCheck.update();                                // 計時器：系統檢查
  //-------------------------------------------------------
}
// ************************************************************************
// ThingSpeak 資料上傳
void ThingSpeak_Upload() {
  //-------------------------------------------------------
  if (strlen(myConfig.ThingSpeak_WriteAPIKey)==0) return; // 沒有設定 WriteAPIKey
  //-------------------------------------------------------
  if (WiFi.status() != WL_CONNECTED) return;              // 網路未連線？
  //-------------------------------------------------------
  WiFi_LED_Ticker.attach(0.1, WiFi_LED_Blink);            // WiFi LED 快速閃爍
  //-------------------------------------------------------
  if (myS3X.NowTemperature()>99)
        ThingSpeak.setField(1,99);
  else  ThingSpeak.setField(1,myS3X.NowTemperature());
  ThingSpeak.setField(2,myS3X.SetTemperature());
  ThingSpeak.setField(3,myS3X.SystemStatusByte());
  ThingSpeak.writeFields(myConfig.ThingSpeak_ChannelNumber, myConfig.ThingSpeak_WriteAPIKey);
  //-------------------------------------------------------
  WiFi_LED_Ticker.detach();                               // WiFi LED 停止閃爍
  digitalWrite(Pin_WiFi_LED, HIGH);                       // 設定連線燈：ON
  //-------------------------------------------------------
  myS3X.Clear_ReceiveBuffer();                            // 因為 ThingSpeak 上傳資料會占用時間，清除接收區資料免得發生封包錯誤
  //-------------------------------------------------------
}
// ************************************************************************
// 系統檢查
void SystemCheck() {
  //-------------------------------------------------------
  if (myS3X.Link_TimeOut()) {                             // 連線愈時
    if (myS3X.NowTemperature()!=0x00) myS3X.ResetData();  // 檢查是否重置過資料
  }
  //-------------------------------------------------------
  if (Link_Blink.isActive()) {                            // 閃爍動作中？
    digitalWrite(Pin_Link_LED,Link_Blink.GetLowHigh());
    Link_Blink.Update();
  }
  //-------------------------------------------------------
}
// ************************************************************************
// OTA 設置
void OTA_Init() {
  ArduinoOTA.setHostname(mDNS_Name);
  ArduinoOTA.onStart([]() {
    digitalWrite(Pin_Link_LED,LOW);                       // 設定 Link 燈：OFF
    WiFi_LED_Ticker.attach(0.5, WiFi_LED_Blink);          // WiFi LED 慢速閃爍
  });
  ArduinoOTA.onEnd([]() {
    WiFi_LED_Ticker.detach();                             // WiFi LED 停止閃爍
    digitalWrite(Pin_WiFi_LED, HIGH);                     // 設定 WiFi 燈：ON
    digitalWrite(Pin_Link_LED, HIGH);                     // 設定 Link 燈：ON
    ESP.restart();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
  });
  ArduinoOTA.onError([](ota_error_t error) {
    /*
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    */
  });
  ArduinoOTA.begin();
}
// ************************************************************************
// Web Server 運行
void WebServer_Run() {
  unsigned long KeepTime = micros();
  WebServer.handleClient();
                                                          // 沒有 Client 要求上傳資料時，其時間差大都在 100 之內
                                                          // 因應 Client 要求上傳資料會占用時間，清除接收區資料免得發生封包錯誤
  if((micros()-KeepTime) > 200) myS3X.Clear_ReceiveBuffer();
}
// ************************************************************************
// Web 網頁：【/】
void Web_Root() {
  String content="";
  content += "<script>";
  content += "var myVar=setInterval(myTimer,1000);";
  content += "var SecCount=0;";
  content += "function myTimer(){";
  content += "SecCount=(SecCount+1)%300;";
  content += "document.getElementById('myProgress').value=Math.round(100*SecCount/300);";
  content += "}";
  content += "</script>";
  content += "<meta charset='UTF-8' http-equiv='refresh' content='300'>";   // 5分鐘(300秒) 自動 refresh
  content += "<html><head><title>" + SlaveName[myConfig.S3X_IdentIndex] + "：" + Temperature_String(myS3X.NowTemperature()) + " ℃ </title></head>";
  content += "<body style='font-family:Consolas'>";
  content += "<table cellpadding='6' border='1'>";
  content += (String)"<tr style='background-color:#A9D0F5'><td colspan='2' align='center'>" + mDNS_Name + "</td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2' align='center'><progress id='myProgress' value='0' max='100' width='100%'></progress></td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td align='center'>目前水溫</td><td align='center'>水溫設定</td></tr>";
  content += "<tr><td align='center'><font size='6' color='red'><strong>" + Temperature_String(myS3X.NowTemperature()) + "</strong></font></td>";
  content += "<td align='center'><font size='6' color='blue'><strong>" + Temperature_String(myS3X.SetTemperature()) + "</strong></font></td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2'>主機(P31)狀態</td></tr>";
  content += "<tr><td colspan='2'>";
  content += (String)"<font color='" + (myS3X.SystemStatusBit(7)==0x00?"black":"red") + "'>" + (myS3X.SystemStatusBit(7)==0x00?"○":"●") + ".8：控制箱未連線</font><br>";
  content += (String)"<font color='" + (myS3X.SystemStatusBit(6)==0x00?"black":"red") + "'>" + (myS3X.SystemStatusBit(6)==0x00?"○":"●") + ".7：溫度異常</font><br>";
  content += (String)"<font color='" + (myS3X.SystemStatusBit(5)==0x00?"black":"red") + "'>" + (myS3X.SystemStatusBit(5)==0x00?"○":"●") + ".6：無法加熱</font><br>";
  content += (String)"<font color='" + (myS3X.SystemStatusBit(4)==0x00?"black":"red") + "'>" + (myS3X.SystemStatusBit(4)==0x00?"○":"●") + ".5：未知 #5</font><br>";
  content += (String)"<font color='" + (myS3X.SystemStatusBit(3)==0x00?"black":"red") + "'>" + (myS3X.SystemStatusBit(3)==0x00?"○":"●") + ".4：加熱中</font><br>";
  content += (String)"<font color='" + (myS3X.SystemStatusBit(2)==0x00?"black":"red") + "'>" + (myS3X.SystemStatusBit(2)==0x00?"○":"●") + ".3：強制加熱模式</font><br>";
  content += (String)"<font color='" + (myS3X.SystemStatusBit(1)==0x00?"black":"red") + "'>" + (myS3X.SystemStatusBit(1)==0x00?"○":"●") + ".2：水位過低</font><br>";
  content += (String)"<font color='" + (myS3X.SystemStatusBit(0)==0x00?"black":"red") + "'>" + (myS3X.SystemStatusBit(0)==0x00?"○":"●") + ".1：未知 #1</font><br>";
  content += "</td></tr>";
  content += (String)"<tr><td colspan='2'>封包錯誤：" + myS3X.PacketErrorCount() + "</td></tr>";
  content += "<tr><td colspan='2'>IP：" + IP_To_String(WiFi.localIP()) + "</td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2' align='center'>";
  content += "<input type='button' value='強制加熱' onclick=\"location.href='/ForcedHeating'\">&emsp;";
  content += "<input type='button' value='水溫設定' onclick=\"location.href='/SetTem'\">";
  content += "</td></tr>";
  content += "</table><br>";
  content += "<input type='button' value='設定' onclick=\"location.href='/Setup'\">&emsp;";
  content += "<input type='button' value='重開機' onclick=\"location.href='/Reboot'\">&emsp;";
  content += "<input type='button' value='Info' onclick=\"location.href='/Info'\">";
  content += "</body></html>";
  WebServer.send(200, "text/html", content);  
}
// ************************************************************************
// Web 網頁：【/Setup】
void Web_Setup() {
  byte CX;
  String msg="";
  String content="";
  //-------------------------------------------------------
  // http://www.blueshop.com.tw/board/FUM200410061525290EW/BRD20050926131412GIB.html
  // checkbox的特性....就是有打勾的才會送出
  //-------------------------------------------------------
  if (WebServer.args()==9 || WebServer.args()==10) {      // 按下【儲存設定】送出表單
    myConfig.S3X_IdentIndex = WebServer.arg("S3X_IdentIndex").toInt();
    myConfig.S3X_RunModeIndex = WebServer.arg("S3X_RunModeIndex").toInt();
    myConfig.RS485_ConfigIndex = WebServer.arg("RS485_ConfigIndex").toInt();
    //-------------------------------------------------------
    myConfig.Disable_AP = WebServer.arg("Disable_AP").toInt();
    //-------------------------------------------------------
    msg = WebServer.arg("WiFi_SSID");
    msg.trim();
    msg.toCharArray(myConfig.WiFi_SSID,sizeof(myConfig.WiFi_SSID));
    //-------------------------------------------------------
    msg = WebServer.arg("WiFi_PASS");
    msg.trim();
    msg.toCharArray(myConfig.WiFi_PASS,sizeof(myConfig.WiFi_PASS));
    //-------------------------------------------------------
    myConfig.ThingSpeak_Upload_MinuteIndex = WebServer.arg("ThingSpeak_Upload_MinuteIndex").toInt();
    myConfig.ThingSpeak_ChannelNumber = WebServer.arg("ThingSpeak_ChannelNumber").toInt();
    //-------------------------------------------------------
    msg = WebServer.arg("ThingSpeak_WriteAPIKey");
    msg.trim();
    msg.toUpperCase();
    msg.toCharArray(myConfig.ThingSpeak_WriteAPIKey,sizeof(myConfig.ThingSpeak_WriteAPIKey));
    //-------------------------------------------------------
    SaveConfig();                                       // 儲存設定
    msg = "儲存完畢 & 請重開機<br>";
  }
  //-------------------------------------------------------
  content += "<meta charset='UTF-8'>";
  content += "<html><body style='font-family:Consolas'><form action='/Setup' method='POST'><br>";
  content += "<table cellpadding='6' border='1'>";
  content += (String)"<tr style='background-color:#A9D0F5'><td colspan='2' align='center'>" + mDNS_Name + "</td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2'>S3X 設定</td></tr>";
  content += "<tr><td>對應設備</td><td>";
  for(CX=0; CX<3; CX++)  
    content += (String)"<input type='radio' name='S3X_IdentIndex' value='" + CX + "'" +(myConfig.S3X_IdentIndex==CX?" checked":"") + ">" + SlaveName[CX];
  content += "</td></tr>";
  content += "<tr><td>運行模式</td><td>";
  content += (String)"<input type='radio' name='S3X_RunModeIndex' value='0'" + (myConfig.S3X_RunModeIndex==0?" checked":"") + ">紀錄模式";
  content += (String)"<input type='radio' name='S3X_RunModeIndex' value='1'" + (myConfig.S3X_RunModeIndex==1?" checked":"") + ">模擬模式";
  content += "</td></tr>";
  content += "<tr><td>通訊組態</td><td>";
  for(CX=0; CX<3; CX++)  
    content += (String)"<input type='radio' name='RS485_ConfigIndex' value='" + CX + "'" +(myConfig.RS485_ConfigIndex==CX?" checked":"") + ">" + RS485_ConfigText[CX];
  content += "</td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2'>無線網路</td></tr>";
  content += (String)"<tr><td>Soft AP</td><td><input type='checkbox' name='Disable_AP' value='1'" + (myConfig.Disable_AP!=0?" checked":"") + ">停用 AP</td></tr>";
  content += (String)"<tr><td>SSID</td><td><input type='text' maxlength='16' name='WiFi_SSID' placeholder='WiFi SSID 名稱' value='" + myConfig.WiFi_SSID + "'></td></tr>";
  content += (String)"<tr><td>密碼</td><td><input type='password' maxlength='16' name='WiFi_PASS' placeholder='WiFi 密碼' value='" + myConfig.WiFi_PASS + "'></td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2'>ThingSpeak 設定</td></tr>";
  content += "<tr><td>資料上傳間隔</td><td>";
  for(CX=0; CX<3; CX++)  
    content += (String)"<input type='radio' name='ThingSpeak_Upload_MinuteIndex' value='" + CX + "'" +(myConfig.ThingSpeak_Upload_MinuteIndex==CX?" checked":"") + ">" + Minute_Interval[CX] + "分鐘";
  content += "</td></tr>";
  content += (String)"<tr><td>Channel Number</td><td><input type='number' min='0' maxlength='10' name='ThingSpeak_ChannelNumber' placeholder='Channel Number' value='" + myConfig.ThingSpeak_ChannelNumber + "'></td></tr>";
  content += (String)"<tr><td>Write API Key</td><td><input type='text' maxlength='16' name='ThingSpeak_WriteAPIKey' placeholder='Write API Key' value='" + myConfig.ThingSpeak_WriteAPIKey + "'></td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2' align='right'>";
  content += "<input type='submit' name='SUBMIT' value='儲存設定'>";
  content += (String)"[" + WebServer.args() + "]";
  content += "</td></tr></table></form>" + msg;
  content += "<input type='button' value='返回主畫面' onclick=\"location.href='/'\">";
  content += "</body></html>";
  WebServer.send(200, "text/html", content);  
}
// ************************************************************************
// Web 網頁：【/SetTem】
void Web_SetTemperature() {
  static byte Sim_SetTemperature;
  String msg="";
  String content="";
  //-------------------------------------------------------
   if (WebServer.args()==2 || WebServer.args()==1) {      // 按下【儲存設定】送出表單 或者 http://s3x-proxy.local/SetTem?SetTemperature=40
    Sim_SetTemperature = WebServer.arg("SetTemperature").toInt();
    myS3X.Sim_SetTemperature(Sim_SetTemperature);
    msg = "更改水溫設定完畢<br>";
   } else {
    Sim_SetTemperature = constrain(myS3X.SetTemperature(),30,75);
   }
  //-------------------------------------------------------
  content += "<meta charset='UTF-8'>";
  content += "<html><body style='font-family:Consolas'><form action='/SetTem' method='POST'>";
  content += "<table cellpadding='6' border='1'>";
  content += (String)"<tr style='background-color:#A9D0F5'><td colspan='2' align='center'>" + mDNS_Name + "</td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2'>水溫設定</td></tr>";
  content += (String)"<tr><td>溫度</td><td><input type='number' min='30' max='75' maxlength='2' name='SetTemperature' placeholder='30~75' value='" + Sim_SetTemperature + "'></td></tr>";
  content += "<tr style='background-color:#DCDCDC'><td colspan='2' align='center'>";
  content += "<input type='submit' name='SUBMIT' value='確認更改'>";
  content += (String)"[" + WebServer.args() + "]";
  content += "</td></tr></table></form>" + msg;
  content += "<input type='button' value='返回主畫面' onclick=\"location.href='/'\">";
  content += "</body></html>";
  WebServer.send(200, "text/html", content);  
  //-------------------------------------------------------
}
// ************************************************************************
// Web Server 設置
void WebServer_Init() {
  WebServer.on("/", Web_Root);                            // 系統資訊
  WebServer.on("/Setup", Web_Setup);                      // 系統設置
  WebServer.on("/SetTem", Web_SetTemperature);            // 水溫設定
  //-------------------------------------------------------
  WebServer.on("/Reboot", [](){                           // 重新開機
    WebServer.send(200, "text/html", "<meta charset='UTF-8' http-equiv='refresh' content='10;url=/'>重新開機<br>稍待 10 秒 返回主頁面...");
    ESP.restart();
  });
  //-------------------------------------------------------
  WebServer.on("/ForcedHeating", [](){                    // 強制加熱
    myS3X.Sim_ForcedHeating();
                                                          // 5秒後轉回主頁面
    WebServer.send(200, "text/html", "<meta charset='UTF-8' http-equiv='refresh' content='5;url=/'>強制加熱【開/關】<br>稍待 5秒 返回主頁面...");
  });
  //-------------------------------------------------------
  WebServer.on("/Info", [](){                             // S3X 資訊
    String JSON_Str="{";
    JSON_Str += (String) "\"S3X\":\"" + SlaveName[myS3X.IdentIndex()] + "\",";
    JSON_Str += (String) "\"TEM\":" + myS3X.NowTemperature() + ",";
    JSON_Str += (String) "\"SET\":" + myS3X.SetTemperature() + ",";
    JSON_Str += (String) "\"SSB\":" + myS3X.SystemStatusByte() + ",";
    JSON_Str += (String) "\"PEC\":" + myS3X.PacketErrorCount() + "}";
    WebServer.send(200, "text/html", JSON_Str);
  });
  //-------------------------------------------------------
  WebServer.begin();
  //-------------------------------------------------------
}
// ************************************************************************
// Wifi LED 閃爍
void WiFi_LED_Blink() {
  digitalWrite(Pin_WiFi_LED, !digitalRead(Pin_WiFi_LED)); 
}
// ************************************************************************
// WiFi 事件
void WiFiEvent(WiFiEvent_t event) {
    switch(event) {
        case WIFI_EVENT_STAMODE_GOT_IP:           // 取得 IP
            WiFi_LED_Ticker.detach();             // WiFi LED 停止閃爍
            digitalWrite(Pin_WiFi_LED, HIGH);     // WiFi LED：ON
            if (myConfig.Disable_AP != 0 && WiFi.softAPIP() != 0) 
              WiFi.mode(WIFI_STA);
            break;
        case WIFI_EVENT_STAMODE_DISCONNECTED:     // 離線
                                                  // WiFi LED 慢速閃爍
            WiFi_LED_Ticker.attach(0.5, WiFi_LED_Blink);  
            break;
    }
}
// ************************************************************************
// WiFi 設置
void WiFi_init() {
  //-------------------------------------------------------
  WiFi.macAddress(WiFi_MAC);                      // 取得 WiFi MAC 資料
                                                  // 設置 Soft AP 的 SSID
  sprintf(AP_SSID,"%s_%02X%02X%02X",mDNS_Name,WiFi_MAC[3],WiFi_MAC[4],WiFi_MAC[5]);
  //sprintf(AP_SSID,"S3X-%02X%02X%02X%02X%02X%02X",WiFi_MAC[0],WiFi_MAC[1],WiFi_MAC[2],WiFi_MAC[3],WiFi_MAC[4],WiFi_MAC[5]);
  //-------------------------------------------------------
  if (myConfig.Disable_AP != 0) {
    WiFi.mode(WIFI_STA);  
  } else {
    WiFi.mode(WIFI_AP_STA);                       // AP + Station 模式
    WiFi.softAP(AP_SSID);                         // 啟用 Soft AP 
  }
  //-------------------------------------------------------                                         
  WiFi.disconnect(true);                          // 一定要做，不燃 WiFi.status() 不會變化
  delay(1000);                                    // 延遲 1 秒
  WiFi.onEvent(WiFiEvent);                        // 設置 WiFi 事件
  WiFi_LED_Ticker.attach(0.5, WiFi_LED_Blink);    // WiFi LED 慢速閃爍
                                                  // 開始連接 WiFi 分享器
  WiFi.begin(myConfig.WiFi_SSID, myConfig.WiFi_PASS);
  //-------------------------------------------------------
  for (byte CX=0; CX<10; CX++) {                  // 10 秒內檢查是否連線成功
    delay(1000);
    if (WiFi.status() == WL_CONNECTED) break;     // 已連線
  }
  //-------------------------------------------------------
  if (WiFi.status() != WL_CONNECTED) {            // 未連線
    WiFi.mode(WIFI_AP_STA);                       // AP + Station 模式
    WiFi.softAP(AP_SSID);                         // 啟用 Soft AP 
  } 
  //-------------------------------------------------------
  MDNS.begin(mDNS_Name);                        // 設置 mDNS
  MDNS.addService("http", "tcp", 80);           // Add service to MDNS-SD，務必要執行，不然 MDNS.queryService 查詢不到
  //-------------------------------------------------------
}
// ************************************************************************
// 設定值：載入
void LoadConfig() {
  //-------------------------------------------------------
  #if defined(ARDUINO_ARCH_ESP8266)   
    EEPROM.begin(sizeof(myConfig));
  #endif
  //-------------------------------------------------------
  for (byte CX=0; CX<sizeof(myConfig); CX++)
      *((byte*)&myConfig + CX) = EEPROM.read( 0 + CX);
  //-------------------------------------------------------
  if ((byte)~myConfig.Head != (byte)myConfig.Tail)// 頭、尾檢核資料有誤
    memset(&myConfig,0,sizeof(myConfig));         // 清除設定資料
  //-------------------------------------------------------
  myS3X.IdentIndex(myConfig.S3X_IdentIndex);      // 設置 S3X IdentIndex，假如 IdentIndex 資料有誤 S3X 會修正
  myConfig.S3X_IdentIndex=myS3X.IdentIndex();     // 取回 IdentIndex
  //-------------------------------------------------------
  myS3X.RunModeIndex(myConfig.S3X_RunModeIndex);  // 設置 S3X RunModeIndex，假如 RunModeIndex 資料有誤 S3X 會修正
  myConfig.S3X_RunModeIndex=myS3X.RunModeIndex(); // 取回 RunModeIndex
  //-------------------------------------------------------
                                                  // 檢查有無超過範圍
  if (myConfig.RS485_ConfigIndex>=3) myConfig.RS485_ConfigIndex=0x00;
  //-------------------------------------------------------
                                                  // 檢查有無超過範圍
  if (myConfig.ThingSpeak_Upload_MinuteIndex>=3) myConfig.ThingSpeak_Upload_MinuteIndex=0x00;
  //-------------------------------------------------------
}
// ************************************************************************
// 設定值：儲存
void SaveConfig() {
  #if defined(ARDUINO_ARCH_ESP8266)   
    EEPROM.begin(sizeof(myConfig));
  #endif
  //-------------------------------------------------------
  myConfig.Head = random(255+1);                  // 取亂數 0~255
  myConfig.Tail = (~myConfig.Head);               // 將值反向(NOT)
  //-------------------------------------------------------
  for (byte CX=0; CX<sizeof(myConfig); CX++)
      EEPROM.write(0 + CX, *((byte*)&myConfig + CX));
  //-------------------------------------------------------
  #if defined(ARDUINO_ARCH_ESP8266)
    EEPROM.commit();                            // 更新至 EEPROM
  #endif
  //-------------------------------------------------------
}
// ************************************************************************
// UART 設置
void UART_Init() {
  //-------------------------------------------------------
  RS485_Port.begin(RS485_Speed,RS485_Config[myConfig.RS485_ConfigIndex]);
  //-------------------------------------------------------
}
// ************************************************************************
// 溫度值轉為字串
String Temperature_String(byte value) {
  String str;

  if (value==0xFF || value==0x00) {
    str = "--";
  } else {
    if (value>99) value=99;
    str = String(value/10) + String(value%10);
  }
  return(str);
}
// ************************************************************************
// IP 轉字串
String IP_To_String(IPAddress ip){
  String IP_Str="";
  for (byte CX=0; CX<4; CX++)
    IP_Str += (CX>0 ? "." + String(ip[CX]) : String(ip[CX]));
  return(IP_Str);
}
// ************************************************************************
// IO 腳位設置
void IO_Init() {
  //-------------------------------------------------------
  pinMode(Pin_Link_LED, OUTPUT);                  // 設定腳位：Link 燈
  digitalWrite(Pin_Link_LED, LOW);
  //-------------------------------------------------------
  pinMode(Pin_WiFi_LED, OUTPUT);                  // 設定腳位：WiFi 燈
  digitalWrite(Pin_WiFi_LED, LOW);
  //-------------------------------------------------------
}
// ************************************************************************
// LED 閃爍
void LED_Blink(byte Pin_LED, unsigned int DelayTime, byte Count) {
  for (unsigned int CX=0; CX<Count; CX++) {
    digitalWrite(Pin_LED, !digitalRead(Pin_LED));         // 設定LED：反向
    delay(DelayTime/2);
    digitalWrite(Pin_LED, !digitalRead(Pin_LED));         // 設定LED：反向
    delay(DelayTime/2);
  }
}
// ************************************************************************
