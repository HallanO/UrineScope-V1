// ============================================================
//  UrineScope v2.1  —  Final Complete Sketch
//  Board  : Arduino Mega 2560
//  Network: SIM800L  (NTP + WeatherAPI + Firebase)
//  Sensors: TDS (A12) ×2 | TCS34725 (I2C) | MQ135 (A9) | MQ3 (A8)
//  Display: ST7789 TFT 240×280 portrait init, rotation 3
//  Buzzer : Active piezo D2
//
//  Scoring weights: TDS 40% | Colour b* 30% | MQ135 17% | MQ3 13%
//  TCF / WCF applied to TDS ONLY.
//  MQ135 (ammonia) and MQ3 (ketones) are metabolic markers —
//  NOT adjusted for time-of-day or ambient weather.
//
//  MQ display: (1 - Rs/R0) × 100% so values RISE as gas rises.
//  Green < 30% | Orange 30-60% | Red > 60%
//
//  State cycle:
//  STANDBY → SETTLING → SAMPLING → DISPLAYING (30s)
//         → FLUSHING (20s) → READY (10s) → STANDBY
// ============================================================
#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include <SoftwareSerial.h>
#include <Adafruit_TCS34725.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

// ── PINS ─────────────────────────────────────────────────────
#define TFT_CS    53
#define TFT_DC    49
#define TFT_RES   48
#define TFT_SCL   52
#define TFT_SDA   51
#define SIM_RX    11
#define SIM_TX    10
#define TDS_PIN   A12
#define MQ135_PIN A9   // Ammonia  — UTI / kidney / liver marker
#define MQ3_PIN   A8   // Acetone  — DKA / ketosis marker
#define BUZZER    2
#define VREF      5.0f

// ── HARDWARE ─────────────────────────────────────────────────
Adafruit_ST7789   tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RES);
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS,
                                            TCS34725_GAIN_4X);
SoftwareSerial SIM800(SIM_RX, SIM_TX);

// ── COLOUR PALETTE ───────────────────────────────────────────
#define BG_COLOR       0x0841
#define CARD_BG        0x1082
#define ACCENT_BLUE    0x3C9F
#define STATUS_ORANGE  0xFD60
#define STATUS_GREEN   0x07E0
#define STATUS_RED     0xF800
#define STATUS_YELLOW  0xFFE0
#define TEXT_PRIMARY   0xFFFF
#define TEXT_SECONDARY 0x8410
#define TEMP_COLOR     0xFFDF
#define HEADER_BG      0x0421

// ── TIMING ───────────────────────────────────────────────────
#define TDS_DETECT_PPM  1500.0f
#define TDS_SUSTAIN_MS  1500UL
#define SETTLING_MS     2500UL
#define SAMPLE_INTERVAL 200UL
#define DISPLAY_MS      30000UL
#define FLUSH_MS        20000UL
#define READY_MS        10000UL
#define WX_INTERVAL     1800000UL  // 30 min

// ── FIREBASE CREDENTIALS ─────────────────────────────────────
#define FB_HOST   "urinescope-6fe77-default-rtdb.firebaseio.com"
#define FB_SECRET "YUhM73dvTgPJc27LNdsA5svBEWY6ZswW7Exi3hAH"
#define FB_PATH   "/readings.json"

// ── WEATHER API ──────────────────────────────────────────────
const char* WX_KEY  = "f0f5c968224a40ffbe574746260105";
const char* WX_CITY = "Kampala";

// ── STATE MACHINE ─────────────────────────────────────────────
enum SysState { STANDBY, SETTLING, SAMPLING, DISPLAYING, FLUSHING, READY };
SysState      sysState    = STANDBY;
unsigned long stateAt     = 0;
bool          screenDrawn = false;

// ── SOFTWARE RTC ─────────────────────────────────────────────
uint8_t  rtc_h=0, rtc_m=0, rtc_s=0;
uint8_t  rtc_day=1, rtc_mon=1;
int      rtc_year=2026;
unsigned long lastRtcTick = 0;
char     g_timeStr[9];
char     g_dateStr[28];

// ── WEATHER ROLLING HISTORY (4 × 30 min = 2 hr window) ───────
struct WxSnap { float temp; uint8_t hum; bool valid; };
WxSnap        wx[4];
uint8_t       wxSlot=0, wxCount=0;
float         wx_temp=25.0f;
uint8_t       wx_hum=50;
char          wx_cond[20]="";
unsigned long lastWxFetch=0;

// ── MQ BASELINES ─────────────────────────────────────────────
float mq135_R0=1.0f, mq3_R0=1.0f;

// ── DETECTION ────────────────────────────────────────────────
unsigned long tdsAboveAt=0;

// ── OVERRIDE FLAGS ───────────────────────────────────────────
#define FLAG_NONE          0
#define FLAG_HEMATURIA     1
#define FLAG_HIGH_AMMONIA  2
#define FLAG_HIGH_KETONES  3
#define FLAG_SEVERE_DEHYD  4
#define FLAG_OVERHYDRATED  5

// ── DIAGNOSIS RESULT ─────────────────────────────────────────
struct DxResult {
  uint8_t  score, flag;
  char     label[22], rec[84];
  uint16_t color;
  float    tds_raw, tds_final;
  float    L, a_val, b_val;
  float    mq135r, mq3r;
  uint8_t  R, G, B;
};
DxResult lastDx;

// ═════════════════════════════════════════════════════════════
//  SECTION 1 — BUZZER
// ═════════════════════════════════════════════════════════════
void beep(uint16_t ms){
  digitalWrite(BUZZER,HIGH); delay(ms); digitalWrite(BUZZER,LOW);
}
void buzzerReady(){ beep(110); delay(100); beep(110); }
void buzzerGood() { beep(420); }
void buzzerMild() { beep(180); delay(110); beep(180); }
void buzzerAlert(){ beep(80); delay(55); beep(80); delay(55); beep(80); }
void playResultBuzzer(const DxResult& dx){
  if(dx.flag!=FLAG_NONE||dx.score>55){ buzzerAlert(); return; }
  if(dx.score>40)                    { buzzerMild();  return; }
  buzzerGood();
}

// ═════════════════════════════════════════════════════════════
//  SECTION 2 — SOFTWARE RTC
// ═════════════════════════════════════════════════════════════
uint8_t dayOfWeek(uint8_t d,uint8_t m,int y){
  static const uint8_t t[] PROGMEM={0,3,2,5,0,3,5,1,4,6,2,4};
  if(m<3) y--;
  return (y+y/4-y/100+y/400+pgm_read_byte(&t[m-1])+d)%7;
}
void tickRTC(){
  if(millis()-lastRtcTick<1000) return;
  lastRtcTick+=1000;
  if(++rtc_s>=60){rtc_s=0;
  if(++rtc_m>=60){rtc_m=0;
  if(++rtc_h>=24){rtc_h=0;
    uint8_t dim[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(rtc_year%4==0&&(rtc_year%100!=0||rtc_year%400==0)) dim[1]=29;
    if(++rtc_day>dim[rtc_mon-1]){rtc_day=1;
    if(++rtc_mon>12){rtc_mon=1;rtc_year++;}}}}}
  sprintf(g_timeStr,"%02d:%02d:%02d",rtc_h,rtc_m,rtc_s);
  const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  const char* mons[]={"Jan","Feb","Mar","Apr","May","Jun",
                       "Jul","Aug","Sep","Oct","Nov","Dec"};
  uint8_t dow=dayOfWeek(rtc_day,rtc_mon,rtc_year);
  sprintf(g_dateStr,"%s -%02d- %s -%04d",
          days[dow],rtc_day,mons[rtc_mon-1],rtc_year);
}

// ═════════════════════════════════════════════════════════════
//  SECTION 3 — SIM800L  (AT commands)
// ═════════════════════════════════════════════════════════════
String sendAT(const String& cmd,unsigned long timeout,
              const String& expect="OK"){
  String r="";
  SIM800.println(cmd);
  unsigned long t=millis();
  while(millis()-t<timeout){
    while(SIM800.available()) r+=(char)SIM800.read();
    if(expect.length()&&r.indexOf(expect)!=-1){delay(20);break;}
  }
  return r;
}

bool openGPRS(){
  for(uint8_t i=0;i<15;i++){
    String r=sendAT(F("AT+CREG?"),2000,F("OK"));
    if(r.indexOf(F(",1"))!=-1||r.indexOf(F(",5"))!=-1) break;
    if(i==14) return false;
    delay(1000);
  }
  sendAT(F("AT+SAPBR=3,1,\"Contype\",\"GPRS\""),2000);
  sendAT(F("AT+SAPBR=3,1,\"APN\",\"internet\""),2000);
  sendAT(F("AT+SAPBR=1,1"),6000);
  return true;
}

void syncTime(){
  if(!openGPRS()) return;
  sendAT(F("AT+CNTP=\"pool.ntp.org\",12"),2000);
  sendAT(F("AT+CNTP"),10000,F("+CNTP: 1"));
  String tr=sendAT(F("AT+CCLK?"),2000);
  int s1=tr.indexOf('"'),s2=tr.lastIndexOf('"');
  if(s1<0||s2<=s1) return;
  String dt=tr.substring(s1+1,s2);
  int comma=dt.indexOf(','); if(comma<0) return;
  String dRaw=dt.substring(0,comma),tFull=dt.substring(comma+1);
  int sign=tFull.indexOf('+'); if(sign<0) sign=tFull.indexOf('-');
  String tOnly=(sign>=0)?tFull.substring(0,sign):tFull;
  rtc_year=2000+dRaw.substring(0,2).toInt();
  rtc_mon =dRaw.substring(3,5).toInt();
  rtc_day =dRaw.substring(6,8).toInt();
  rtc_h   =tOnly.substring(0,2).toInt();
  rtc_m   =tOnly.substring(3,5).toInt();
  rtc_s   =tOnly.substring(6,8).toInt();
}

void fetchWeather(){
  sendAT(F("AT+HTTPTERM"),500);
  delay(100);
  sendAT(F("AT+HTTPINIT"),1000);
  sendAT(F("AT+HTTPPARA=\"CID\",1"),1000);
  String url=String(F("AT+HTTPPARA=\"URL\",\"http://api.weatherapi.com/v1/current.json?key="));
  url+=WX_KEY; url+=F("&q="); url+=WX_CITY; url+=F("&aqi=no\"");
  sendAT(url,1500);
  sendAT(F("AT+HTTPACTION=0"),15000,F("+HTTPACTION:"));
  String p=sendAT(F("AT+HTTPREAD"),8000);
  sendAT(F("AT+HTTPTERM"),1000);

  int idx=p.indexOf(F("\"temp_c\":"));
  if(idx>=0){int c=p.indexOf(',',idx);wx_temp=p.substring(idx+9,c).toFloat();}
  idx=p.indexOf(F("\"humidity\":"));
  if(idx>=0){int c=p.indexOf(',',idx);if(c<0)c=p.indexOf('}',idx);
    wx_hum=(uint8_t)p.substring(idx+11,c).toInt();}
  String cs=F("\"condition\":{\"text\":\"");
  idx=p.indexOf(cs);
  if(idx>=0){int st=idx+cs.length(),en=p.indexOf('"',st);
    if(en>st) p.substring(st,en).toCharArray(wx_cond,sizeof(wx_cond));}

  wx[wxSlot]={wx_temp,wx_hum,true};
  wxSlot=(wxSlot+1)%4;
  if(wxCount<4) wxCount++;
}

// ═════════════════════════════════════════════════════════════
//  SECTION 4 — FIREBASE POST
// ═════════════════════════════════════════════════════════════
const char* flagName(uint8_t f){
  switch(f){
    case FLAG_HEMATURIA:    return "HEMATURIA";
    case FLAG_HIGH_AMMONIA: return "HIGH_AMMONIA";
    case FLAG_HIGH_KETONES: return "HIGH_KETONES";
    case FLAG_SEVERE_DEHYD: return "SEVERE_DEHYDRATION";
    case FLAG_OVERHYDRATED: return "OVERHYDRATION";
    default:                return "NONE";
  }
}

uint16_t buildJSON(char* buf, uint16_t maxLen, const DxResult& dx){
  char ts[22];
  sprintf(ts,"20%02d-%02d-%02dT%02d:%02d:%02d",
          rtc_year-2000,rtc_mon,rtc_day,rtc_h,rtc_m,rtc_s);
  char dtime[10];
  sprintf(dtime,"%02d:%02d:%02d",rtc_h,rtc_m,rtc_s);

  char rec[84];
  strncpy(rec,dx.rec,83); rec[83]='\0';
  for(uint8_t i=0;rec[i];i++) if(rec[i]=='"') rec[i]='\'';

  char s_tdsRaw[10], s_tdsFin[10];
  char s_L[8], s_a[8], s_b[8];
  char s_mq135[8], s_mq3[8];
  char s_temp[8];

  dtostrf(dx.tds_raw,  1, 0, s_tdsRaw);
  dtostrf(dx.tds_final,1, 0, s_tdsFin);
  dtostrf(dx.L,        1, 1, s_L);
  dtostrf(dx.a_val,    1, 1, s_a);
  dtostrf(dx.b_val,    1, 1, s_b);
  dtostrf(dx.mq135r,   1, 3, s_mq135);
  dtostrf(dx.mq3r,     1, 3, s_mq3);
  dtostrf(wx_temp,     1, 1, s_temp);

  int n=snprintf(buf,maxLen,
    "{"
      "\"ts\":\"%s\","
      "\"device_time\":\"%s\","
      "\"label\":\"%s\","
      "\"rec\":\"%s\","
      "\"score\":%d,"
      "\"flag\":\"%s\","
      "\"tds_raw\":%s,"
      "\"tds_final\":%s,"
      "\"R\":%d,\"G\":%d,\"B\":%d,"
      "\"L\":%s,\"a_val\":%s,\"b_val\":%s,"
      "\"mq135r\":%s,"
      "\"mq3r\":%s,"
      "\"temp\":%s,"
      "\"hum\":%d,"
      "\"cond\":\"%s\""
    "}",
    ts,dtime,
    dx.label,rec,
    dx.score,
    flagName(dx.flag),
    s_tdsRaw,s_tdsFin,
    dx.R,dx.G,dx.B,
    s_L,s_a,s_b,
    s_mq135,s_mq3,
    s_temp,(int)wx_hum,wx_cond
  );
  return (n>0&&n<(int)maxLen)?(uint16_t)n:0;
}

bool postToFirebase(const DxResult& dx){
  char jsonBuf[512];
  uint16_t jsonLen = buildJSON(jsonBuf, sizeof(jsonBuf), dx);
  if(jsonLen == 0){ Serial.println(F("[FB] FAIL: buildJSON")); return false; }
  String payload = String(jsonBuf);
  Serial.print(F("[FB] JSON (")); Serial.print(jsonLen);
  Serial.print(F(" bytes): ")); Serial.println(payload);

  String brChk = sendAT(F("AT+SAPBR=2,1"), 2000);
  Serial.print(F("[FB] Bearer status: ")); Serial.println(brChk);
  if(brChk.indexOf(F("0.0.0.0")) != -1 || brChk.indexOf(F("ERROR")) != -1){
    Serial.println(F("[FB] Bearer closed — reopening GPRS..."));
    if(!openGPRS()){
      Serial.println(F("[FB] FAIL: cannot reopen GPRS"));
      return false;
    }
    Serial.println(F("[FB] GPRS bearer reopened OK"));
  }

  sendAT(F("AT+HTTPTERM"), 1000);
  delay(100);

  String initR = sendAT(F("AT+HTTPINIT"), 2000);
  Serial.print(F("[FB] HTTPINIT: ")); Serial.println(initR);

  sendAT(F("AT+HTTPPARA=\"CID\",1"), 1000);

  String sslR = sendAT(F("AT+HTTPSSL=1"), 1000);
  bool sslOK = (sslR.indexOf(F("OK")) != -1);
  Serial.print(F("[FB] HTTPSSL=1: ")); Serial.println(sslR);
  if(!sslOK){
    Serial.println(F("[FB] WARNING: SSL not supported by this module!"));
  }

  char urlCmd[220];
  snprintf(urlCmd, sizeof(urlCmd),
    "AT+HTTPPARA=\"URL\",\"https://%s%s?auth=%s\"",
    FB_HOST, FB_PATH, FB_SECRET);
  sendAT(String(urlCmd), 1500);

  sendAT(F("AT+HTTPPARA=\"CONTENT\",\"application/json\""), 1000);

  String dataCmd = "AT+HTTPDATA=" + String(jsonLen) + ",10000";
  String dataResp = sendAT(dataCmd, 3000, F("DOWNLOAD"));
  Serial.print(F("[FB] HTTPDATA: ")); Serial.println(dataResp);
  if(dataResp.indexOf(F("DOWNLOAD")) == -1){
    sendAT(F("AT+HTTPTERM"), 1000);
    Serial.println(F("[FB] FAIL: no DOWNLOAD prompt"));
    return false;
  }

  String payloadResp = sendAT(payload, 3000, F("OK"));
  Serial.print(F("[FB] Payload resp: ")); Serial.println(payloadResp);

  String postResp = sendAT(F("AT+HTTPACTION=1"), 15000, F("+HTTPACTION:"));
  Serial.print(F("[FB] HTTPACTION: ")); Serial.println(postResp);

  String readResp = sendAT(F("AT+HTTPREAD"), 5000);
  Serial.print(F("[FB] Response body: ")); Serial.println(readResp);

  sendAT(F("AT+HTTPTERM"), 1000);
  sendAT(F("AT+HTTPSSL=0"), 500);

  bool success = (postResp.indexOf(F(",200,")) != -1);
  Serial.println(success ? F("[FB] POST SUCCESS!") : F("[FB] POST FAILED"));
  return success;
}

// ═════════════════════════════════════════════════════════════
//  SECTION 5 — WEATHER CORRECTION HELPERS
// ═════════════════════════════════════════════════════════════
float heatIdx(float T,float RH){
  return T-(0.55f-0.0055f*RH)*(T-14.5f);
}
float avgHI(){
  if(wxCount==0) return 25.0f;
  if(wxCount==1){
    for(uint8_t i=0;i<4;i++)
      if(wx[i].valid) return heatIdx(wx[i].temp,wx[i].hum);
  }
  float s=0; uint8_t n=0;
  for(uint8_t i=0;i<4;i++){
    if(!wx[i].valid) continue;
    s+=heatIdx(wx[i].temp,wx[i].hum); n++;
  }
  return s/n;
}
float avgRH(){
  if(wxCount==0) return 50.0f;
  float s=0; uint8_t n=0;
  for(uint8_t i=0;i<4;i++){
    if(!wx[i].valid) continue; s+=wx[i].hum; n++;
  }
  return s/n;
}

// ═════════════════════════════════════════════════════════════
//  SECTION 6 — SENSOR READING
// ═════════════════════════════════════════════════════════════
float readTDS(){
  int buf[10];
  for(uint8_t i=0;i<10;i++){buf[i]=analogRead(TDS_PIN);delay(5);}
  for(uint8_t i=0;i<9;i++)
    for(uint8_t j=i+1;j<10;j++)
      if(buf[i]>buf[j]){int t=buf[i];buf[i]=buf[j];buf[j]=t;}
  long s=0; for(uint8_t i=2;i<8;i++) s+=buf[i];
  float v=(s/6.0f)*(VREF/1024.0f);
  float raw=(133.42f*v*v*v-255.86f*v*v+857.39f*v)*0.5f;
  return raw*2.0f;
}

float mqRs(uint8_t pin){
  float v=analogRead(pin)*(VREF/1023.0f);
  if(v<0.01f) v=0.01f;
  return (VREF-v)/v;
}
void calibrateMQ(){
  float r135=0,r3=0;
  for(uint8_t i=0;i<20;i++){
    r135+=mqRs(MQ135_PIN); r3+=mqRs(MQ3_PIN); delay(50);
  }
  mq135_R0=r135/20.0f; if(mq135_R0<0.01f) mq135_R0=0.01f;
  mq3_R0  =r3  /20.0f; if(mq3_R0  <0.01f) mq3_R0  =0.01f;
}
float getMQRatio(uint8_t pin,float R0){ return mqRs(pin)/R0; }

// ═════════════════════════════════════════════════════════════
//  SECTION 7 — RGB → CIE L*a*b*
// ═════════════════════════════════════════════════════════════
static float labF(float t){
  return (t>0.008856f)?cbrt(t):(7.787f*t+16.0f/116.0f);
}
void rgbToLab(uint8_t R,uint8_t G,uint8_t B,
              float&L,float&a,float&bv){
  float r=R/255.0f,g=G/255.0f,b=B/255.0f;
  r=(r>0.04045f)?pow((r+0.055f)/1.055f,2.4f):r/12.92f;
  g=(g>0.04045f)?pow((g+0.055f)/1.055f,2.4f):g/12.92f;
  b=(b>0.04045f)?pow((b+0.055f)/1.055f,2.4f):b/12.92f;
  float X=0.4124f*r+0.3576f*g+0.1805f*b;
  float Y=0.2126f*r+0.7152f*g+0.0722f*b;
  float Z=0.0193f*r+0.1192f*g+0.9505f*b;
  float fx=labF(X/0.9505f),fy=labF(Y),fz=labF(Z/1.0890f);
  L=116.0f*fy-16.0f; a=500.0f*(fx-fy); bv=200.0f*(fy-fz);
}

// ═════════════════════════════════════════════════════════════
//  SECTION 8 — SCORING ENGINE
// ═════════════════════════════════════════════════════════════
float timeCF(uint8_t h){
  if(h>=5 &&h<9)  return 0.75f;
  if(h>=9 &&h<12) return 0.88f;
  if(h>=12&&h<17) return 1.00f;
  if(h>=17&&h<21) return 0.92f;
  return 0.80f;
}
float wxCF(float hi,float rh){
  float w=1.0f;
  if     (hi>35.0f) w=0.70f;
  else if(hi>30.0f) w=0.82f;
  else if(hi>25.0f) w=0.91f;
  if(rh>80.0f) w*=0.88f;
  return w;
}
uint8_t scoreTDS(float t){
  if(t<3000)  return 0;
  if(t<7000)  return 1;
  if(t<10500) return 2;
  return 3;
}
uint8_t scoreColor(float bv){
  if(bv<15.0f) return 0;
  if(bv<30.0f) return 1;
  if(bv<45.0f) return 2;
  return 3;
}
uint8_t scoreMQ135(float ratio){
  if(ratio>1.10f) return 0;
  if(ratio>0.70f) return 1;
  if(ratio>0.40f) return 2;
  return 3;
}
uint8_t scoreMQ3(float ratio){
  if(ratio>1.10f) return 0;
  if(ratio>0.70f) return 1;
  if(ratio>0.40f) return 2;
  return 3;
}
uint8_t checkFlags(float tf,float L,float av,float bv,
                   float r135,float r3){
  if(av>12.0f&&L<55.0f)  return FLAG_HEMATURIA;
  if(r135<0.35f)          return FLAG_HIGH_AMMONIA;
  if(r3<0.35f)            return FLAG_HIGH_KETONES;
  if(tf>13000&&bv>50.0f) return FLAG_SEVERE_DEHYD;
  if(tf<1500 &&bv<8.0f)  return FLAG_OVERHYDRATED;
  return FLAG_NONE;
}
uint8_t compositeScore(uint8_t st,uint8_t sc,
                       uint8_t sm135,uint8_t sm3){
  float w=0.40f*max(st,  (uint8_t)1)
         +0.30f*max(sc,  (uint8_t)1)
         +0.17f*max(sm135,(uint8_t)1)
         +0.13f*max(sm3,  (uint8_t)1);
  return (uint8_t)min((w/3.0f)*100.0f,100.0f);
}
DxResult runDiagnostics(float tds_raw,uint8_t R,uint8_t G,uint8_t B,
                        float r135,float r3){
  DxResult dx;
  dx.tds_raw=tds_raw; dx.R=R; dx.G=G; dx.B=B;
  dx.mq135r=r135; dx.mq3r=r3;
  float hi=avgHI(),rh=avgRH();
  dx.tds_final=tds_raw*timeCF(rtc_h)*wxCF(hi,rh);
  rgbToLab(R,G,B,dx.L,dx.a_val,dx.b_val);
  uint8_t st   =scoreTDS(dx.tds_final);
  uint8_t sc   =scoreColor(dx.b_val);
  uint8_t sm135=scoreMQ135(r135);
  uint8_t sm3  =scoreMQ3(r3);
  dx.flag =checkFlags(dx.tds_final,dx.L,dx.a_val,dx.b_val,r135,r3);
  dx.score=compositeScore(st,sc,sm135,sm3);

  if     (dx.flag==FLAG_HEMATURIA)  {
    strcpy_P(dx.label,PSTR("BLOOD DETECTED"));
    strcpy_P(dx.rec,  PSTR("Blood in urine. See a doctor today - do not delay."));
    dx.color=STATUS_RED; }
  else if(dx.flag==FLAG_HIGH_AMMONIA){
    strcpy_P(dx.label,PSTR("HIGH AMMONIA"));
    strcpy_P(dx.rec,  PSTR("Possible UTI or kidney issue. See a doctor soon."));
    dx.color=STATUS_ORANGE; }
  else if(dx.flag==FLAG_HIGH_KETONES){
    strcpy_P(dx.label,PSTR("HIGH KETONES"));
    strcpy_P(dx.rec,  PSTR("Check blood glucose. If diabetic, seek care today."));
    dx.color=STATUS_ORANGE; }
  else if(dx.flag==FLAG_SEVERE_DEHYD){
    strcpy_P(dx.label,PSTR("SEVERE DEHYDRATION"));
    strcpy_P(dx.rec,  PSTR("Drink water now. Seek help if dizzy or nauseous."));
    dx.color=STATUS_RED; }
  else if(dx.flag==FLAG_OVERHYDRATED){
    strcpy_P(dx.label,PSTR("OVERHYDRATION"));
    strcpy_P(dx.rec,  PSTR("Reduce fluid intake. Persistent? See a doctor."));
    dx.color=ACCENT_BLUE; }
  else if(dx.score<=20){
    strcpy_P(dx.label,PSTR("OPTIMAL"));
    strcpy_P(dx.rec,  PSTR("Excellent hydration and chemistry. Keep it up!"));
    dx.color=STATUS_GREEN; }
  else if(dx.score<=40){
    strcpy_P(dx.label,PSTR("NORMAL"));
    strcpy_P(dx.rec,  PSTR("All parameters within healthy range."));
    dx.color=STATUS_GREEN; }
  else if(dx.score<=55){
    strcpy_P(dx.label,PSTR("MILD CONCERN"));
    strcpy_P(dx.rec,  PSTR("Drink 1-2 glasses of water. Monitor today."));
    dx.color=STATUS_YELLOW; }
  else if(dx.score<=70){
    strcpy_P(dx.label,PSTR("DEHYDRATED"));
    strcpy_P(dx.rec,  PSTR("Increase fluid intake significantly today."));
    dx.color=STATUS_ORANGE; }
  else if(dx.score<=85){
    strcpy_P(dx.label,PSTR("SIGNIFICANT"));
    strcpy_P(dx.rec,  PSTR("Possible health concern. See a doctor if persistent."));
    dx.color=STATUS_RED; }
  else{
    strcpy_P(dx.label,PSTR("URGENT"));
    strcpy_P(dx.rec,  PSTR("Multiple parameters abnormal. Seek medical attention."));
    dx.color=STATUS_RED; }
  return dx;
}

// ═════════════════════════════════════════════════════════════
//  SECTION 9 — DISPLAY HELPERS
// ═════════════════════════════════════════════════════════════
void drawRoundRect(int x,int y,int w,int h,int r,uint16_t c){
  tft.fillRect(x+r,y,w-2*r,h,c);
  tft.fillRect(x,y+r,w,h-2*r,c);
  tft.fillCircle(x+r,    y+r,    r,c);
  tft.fillCircle(x+w-r-1,y+r,    r,c);
  tft.fillCircle(x+r,    y+h-r-1,r,c);
  tft.fillCircle(x+w-r-1,y+h-r-1,r,c);
}

// ═════════════════════════════════════════════════════════════
//  SECTION 10 — STANDBY / HOME SCREEN
// ═════════════════════════════════════════════════════════════
const int MID_Y=95, CARD_W=82, CARD_SP=7;
const int BOT_Y=185,BOT_W=60, BOT_SP=6;

void drawMidCardBase(uint8_t idx,const __FlashStringHelper* title){
  int x=10+idx*(CARD_W+CARD_SP);
  drawRoundRect(x,MID_Y,CARD_W,80,10,CARD_BG);
  tft.setFont(&FreeSansBold9pt7b); tft.setTextColor(ACCENT_BLUE);
  int16_t x1,y1; uint16_t w,h;
  tft.getTextBounds(title,0,0,&x1,&y1,&w,&h);
  tft.setCursor(x+(CARD_W-(int)w)/2,MID_Y+24);
  tft.print(title);
}
void drawBottomCardBase(uint8_t idx,const __FlashStringHelper* title){
  int x=10+idx*(BOT_W+BOT_SP);
  drawRoundRect(x,BOT_Y,BOT_W,55,8,CARD_BG);
  tft.setFont(); tft.setTextSize(1); tft.setTextColor(ACCENT_BLUE);
  tft.setCursor(x+(BOT_W-(int)strlen_P((const char*)title)*6)/2,BOT_Y+8);
  tft.print(title);
}

bool standbyBaseDrawn=false;

void drawStandbyBase(){
  tft.fillScreen(BG_COLOR);
  tft.setFont(&FreeSansBold24pt7b); tft.setTextColor(TEXT_PRIMARY);
  tft.setCursor(66,42);  tft.print(':');
  tft.setCursor(138,42); tft.print(':');
  tft.setFont(&FreeSansBold9pt7b); tft.setTextColor(ACCENT_BLUE);
  tft.setCursor(12,85); tft.print(WX_CITY);
  drawMidCardBase(0,F(""));
  drawMidCardBase(1,F(""));
  drawMidCardBase(2,F("TDS"));
  drawBottomCardBase(0,F("RED"));
  drawBottomCardBase(1,F("GREEN"));
  drawBottomCardBase(2,F("BLUE"));
  drawBottomCardBase(3,F("COLOR"));
  standbyBaseDrawn=true;
  screenDrawn=true;
}

static int8_t   pHour=-1,pMin=-1,pSec=-1;
static char     pDate[28]="",pCond[20]="";
static float    pTemp=-999; static int16_t pHum=-1;
static float    pTds=-999,pMq135=-999,pMq3=-999;
static int16_t  pR=-1,pG=-1,pB=-1;
static uint16_t pCircleColor=0;

void resetStandbyCache(){
  pHour=pMin=pSec=-1; pDate[0]=pCond[0]='\0';
  pTemp=-999;pHum=-1;pTds=pMq135=pMq3=-999;
  pR=pG=pB=-1;pCircleColor=0;
}
void clearMidLine(uint8_t idx,int lineY,int lineH=16){
  tft.fillRect(10+idx*(CARD_W+CARD_SP)+3,lineY,CARD_W-6,lineH,CARD_BG);
}
void clearBotValue(uint8_t idx){
  tft.fillRect(10+idx*(BOT_W+BOT_SP)+3,BOT_Y+24,BOT_W-6,26,CARD_BG);
}

void updateStandby(float tds){
  int16_t x1,y1; uint16_t w,h;

  // Clock
  tft.setFont(&FreeSansBold24pt7b);
  char buf[3];
  auto drawDigit=[&](int8_t&prev,uint8_t val,int cx){
    if(prev==(int8_t)val) return;
    if(prev>=0){
      sprintf(buf,"%02d",(int)prev);
      tft.getTextBounds(buf,cx,42,&x1,&y1,&w,&h);
      tft.fillRect(x1-2,y1-2,w+4,h+4,BG_COLOR);
    }
    sprintf(buf,"%02d",(int)val);
    tft.setTextColor(TEXT_PRIMARY);
    tft.setCursor(cx,42); tft.print(buf);
    prev=(int8_t)val;
  };
  drawDigit(pHour,rtc_h,12);
  drawDigit(pMin, rtc_m,82);
  drawDigit(pSec, rtc_s,154);

  // Date
  if(strcmp(pDate,g_dateStr)){
    if(pDate[0]){
      tft.setFont(&FreeSans9pt7b);
      tft.getTextBounds(pDate,0,0,&x1,&y1,&w,&h);
      tft.getTextBounds(pDate,270-(int)w,60,&x1,&y1,&w,&h);
      tft.fillRect(x1-2,y1-2,w+4,h+4,BG_COLOR);
    }
    tft.setFont(&FreeSans9pt7b);
    tft.getTextBounds(g_dateStr,0,0,&x1,&y1,&w,&h);
    tft.setTextColor(0xCE59);
    tft.setCursor(270-(int)w,60); tft.print(g_dateStr);
    strncpy(pDate,g_dateStr,sizeof(pDate));
  }

  // Weather condition
  if(strcmp(pCond,wx_cond)&&wx_cond[0]){
    if(pCond[0]){
      tft.setFont(&FreeSans9pt7b);
      tft.getTextBounds(pCond,0,0,&x1,&y1,&w,&h);
      tft.getTextBounds(pCond,270-(int)w,85,&x1,&y1,&w,&h);
      tft.fillRect(x1-2,y1-2,w+4,h+4,BG_COLOR);
    }
    tft.setFont(&FreeSans9pt7b);
    tft.getTextBounds(wx_cond,0,0,&x1,&y1,&w,&h);
    tft.setTextColor(STATUS_YELLOW);
    tft.setCursor(270-(int)w,85); tft.print(wx_cond);
    strncpy(pCond,wx_cond,sizeof(pCond));
  }

  // WEATHER card
  if(fabsf(pTemp-wx_temp)>0.09f||pHum!=(int16_t)wx_hum){
    int cx=10;
    clearMidLine(0,MID_Y+20,18);
    tft.setFont(&FreeSans9pt7b);
    char tmp[8];
    tft.setTextColor(ACCENT_BLUE);
    tft.setCursor(cx+5,MID_Y+34); tft.print(F("T: "));
    tft.setTextColor(TEMP_COLOR);
    dtostrf(wx_temp,4,1,tmp); tft.print(tmp); tft.print('C');
    clearMidLine(0,MID_Y+42,18);
    tft.setTextColor(ACCENT_BLUE);
    tft.setCursor(cx+5,MID_Y+56); tft.print(F("H: "));
    tft.setTextColor(TEXT_PRIMARY);
    tft.print(wx_hum); tft.print('%');
    pTemp=wx_temp; pHum=wx_hum;
  }

  // ── AMMONIA card ─────────────────────────────────────────────
  // FIX: display (1 - Rs/R0) × 100 so percentage RISES as gas rises.
  // Clean air → ratio ≈ 1.0 → display ≈ 0%
  // Elevated gas → ratio drops → display rises toward 100%
  // Colour bands: green < 30% | orange 30–60% | red > 60%
  float r135 = max(0.0f, (1.0f - getMQRatio(MQ135_PIN, mq135_R0))) * 100.0f;
  float r3   = max(0.0f, (1.0f - getMQRatio(MQ3_PIN,   mq3_R0  ))) * 100.0f;

  if(fabsf(pMq135-r135)>1.0f||fabsf(pMq3-r3)>1.0f){
    int cx=10+CARD_W+CARD_SP;
    clearMidLine(1,MID_Y+20,18);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(ACCENT_BLUE);
    tft.setCursor(cx+3,MID_Y+34); tft.print(F("NH3:"));
    uint16_t c135=(r135>60)?STATUS_RED:(r135>30)?STATUS_ORANGE:STATUS_GREEN;
    tft.setTextColor(c135);
    tft.print((int)r135); tft.print('%');
    clearMidLine(1,MID_Y+42,18);
    tft.setTextColor(ACCENT_BLUE);
    tft.setCursor(cx+3,MID_Y+56); tft.print(F("KET:"));
    uint16_t c3=(r3>60)?STATUS_RED:(r3>30)?STATUS_ORANGE:STATUS_GREEN;
    tft.setTextColor(c3);
    tft.print((int)r3); tft.print('%');
    pMq135=r135; pMq3=r3;
  }

  // TDS card
  if(fabsf(pTds-tds)>5.0f){
    int cx=10+2*(CARD_W+CARD_SP);
    clearMidLine(2,MID_Y+36,34);
    char ts[8]; dtostrf(tds,0,0,ts);
    tft.setFont(&FreeSansBold9pt7b);
    tft.getTextBounds(ts,0,0,&x1,&y1,&w,&h);
    tft.setTextColor(tds>TDS_DETECT_PPM?STATUS_ORANGE:STATUS_GREEN);
    tft.setCursor(cx+(CARD_W-(int)w)/2,MID_Y+55); tft.print(ts);
    tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_SECONDARY);
    tft.getTextBounds("ppm",0,0,&x1,&y1,&w,&h);
    tft.setCursor(cx+(CARD_W-(int)w)/2,MID_Y+70); tft.print(F("ppm"));
    pTds=tds;
  }

  // Bottom RGB cards
  float rf,gf,bf; tcs.getRGB(&rf,&gf,&bf);
  int16_t R=(int16_t)rf,G=(int16_t)gf,B=(int16_t)bf;
  auto drawBotVal=[&](uint8_t idx,int16_t&prev,int16_t val,uint16_t col){
    if(prev==val) return;
    clearBotValue(idx);
    char cb[5]; sprintf(cb,"%3d",val);
    tft.setFont(&FreeSans9pt7b);
    tft.getTextBounds(cb,0,0,&x1,&y1,&w,&h);
    int bx=10+idx*(BOT_W+BOT_SP);
    tft.setTextColor(col);
    tft.setCursor(bx+(BOT_W-(int)w)/2,BOT_Y+42); tft.print(cb);
    prev=val;
  };
  drawBotVal(0,pR,R,STATUS_RED);
  drawBotVal(1,pG,G,STATUS_GREEN);
  drawBotVal(2,pB,B,0x001F);

  uint16_t circleCol=tft.color565((uint8_t)R,(uint8_t)G,(uint8_t)B);
  if(pCircleColor!=circleCol){
    int bx=10+3*(BOT_W+BOT_SP);
    tft.fillCircle(bx+BOT_W/2,BOT_Y+32,14,circleCol);
    tft.drawCircle(bx+BOT_W/2,BOT_Y+32,15,TEXT_SECONDARY);
    pCircleColor=circleCol;
  }
}

// ═════════════════════════════════════════════════════════════
//  SECTION 11 — ANALYSING SCREEN
// ═════════════════════════════════════════════════════════════
void drawAnalysingScreen(){
  tft.fillScreen(BG_COLOR); tft.setTextSize(1);
  int16_t x1,y1; uint16_t w,h;
  tft.setFont(&FreeSansBold18pt7b);
  tft.getTextBounds("ANALYSING...",0,0,&x1,&y1,&w,&h);
  tft.setTextColor(ACCENT_BLUE);
  tft.setCursor((280-(int)w)/2,128); tft.print(F("ANALYSING..."));
  tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_SECONDARY);
  tft.getTextBounds("Sample detected. Please stand by.",0,0,&x1,&y1,&w,&h);
  tft.setCursor((280-(int)w)/2,148);
  tft.print(F("Sample detected. Please stand by."));
}

// ═════════════════════════════════════════════════════════════
//  SECTION 12 — RESULTS SCREEN
// ═════════════════════════════════════════════════════════════
void drawResultsScreen(const DxResult& dx){
  tft.fillScreen(BG_COLOR); tft.setTextSize(1);
  int16_t x1,y1; uint16_t w,h;

  tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_PRIMARY);
  tft.getTextBounds(g_timeStr,0,0,&x1,&y1,&w,&h);
  tft.setCursor(244-(int)w,20); tft.print(g_timeStr);

  tft.setFont(&FreeSansBold9pt7b); tft.setTextColor(ACCENT_BLUE);
  tft.setCursor(12,50); tft.print(F("ANALYSIS COMPLETE"));

  tft.setTextColor(dx.color);
  tft.setCursor(12,80); tft.print(dx.label);
  tft.getTextBounds(dx.label,0,0,&x1,&y1,&w,&h);
  char sc[14]; sprintf(sc,"SCR : %d %%",dx.score);
  tft.setCursor(12+(int)w+15,80); tft.print(sc);

  const int MY=100, CW=82, CSP=7;
  auto drawMidCol=[&](uint8_t idx,const char* title,
                      const char* val,uint16_t vCol){
    int cx=10+idx*(CW+CSP);
    drawRoundRect(cx,MY,CW,80,10,CARD_BG);
    tft.setFont(&FreeSansBold9pt7b); tft.setTextColor(ACCENT_BLUE);
    tft.getTextBounds(title,0,0,&x1,&y1,&w,&h);
    tft.setCursor(cx+(CW-(int)w)/2,MY+24); tft.print(title);
    tft.setFont(&FreeSansBold9pt7b); tft.setTextColor(vCol);
    tft.getTextBounds(val,0,0,&x1,&y1,&w,&h);
    tft.setCursor(cx+(CW-(int)w)/2,MY+54); tft.print(val);
  };

  char vb[12]; dtostrf(dx.tds_final,0,0,vb); strcat(vb," ppm");
  uint8_t stT=scoreTDS(dx.tds_final);
  drawMidCol(0,"TDS",vb,
    stT>=3?STATUS_RED:stT==2?STATUS_ORANGE:STATUS_GREEN);

  char bvb[8]; sprintf(bvb,"b*: %d",(int)dx.b_val);
  uint8_t stC=scoreColor(dx.b_val);
  drawMidCol(1,"COLOUR",bvb,
    stC>=3?STATUS_RED:stC==2?STATUS_ORANGE:STATUS_GREEN);

  uint8_t stA=scoreMQ135(dx.mq135r),stK=scoreMQ3(dx.mq3r);
  const char* gasLabel;
  uint16_t    gasColor;
  if(dx.flag==FLAG_HIGH_AMMONIA)     { gasLabel="HIGH NH3"; gasColor=STATUS_RED; }
  else if(dx.flag==FLAG_HIGH_KETONES){ gasLabel="HIGH KET"; gasColor=STATUS_RED; }
  else{
    uint8_t worst=max(stA,stK);
    gasLabel=worst>=3?"Elevated":worst==2?"Mild":"Normal";
    gasColor=worst>=3?STATUS_RED:worst==2?STATUS_ORANGE:STATUS_GREEN;
  }
  drawMidCol(2,"GASES",gasLabel,gasColor);

  const int BY=185;
  tft.setFont(&FreeSansBold9pt7b); tft.setTextColor(TEXT_PRIMARY);
  char line1[42]="", line2[42]="";
  strncpy(line1,dx.rec,41); line1[41]='\0';
  if(strlen(dx.rec)>28){
    for(int i=28;i>=0;i--){
      if(line1[i]==' '){
        strncpy(line2,dx.rec+i+1,41); line2[41]='\0';
        line1[i]='\0'; break;
      }
    }
  }
  tft.setCursor(12,BY+20); tft.print(line1);
  if(line2[0]){ tft.setCursor(12,BY+40); tft.print(line2); }
}

// ═════════════════════════════════════════════════════════════
//  SECTION 13 — FLUSH SCREEN
// ═════════════════════════════════════════════════════════════
void drawFlushScreen(){
  tft.fillScreen(BG_COLOR); tft.setTextSize(1);
  int16_t x1,y1; uint16_t w,h;
  tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_PRIMARY);
  tft.getTextBounds(g_timeStr,0,0,&x1,&y1,&w,&h);
  tft.setCursor(244-(int)w,20); tft.print(g_timeStr);
  tft.setFont(&FreeSansBold18pt7b); tft.setTextColor(STATUS_ORANGE);
  tft.getTextBounds("FLUSH",0,0,&x1,&y1,&w,&h);
  tft.setCursor((280-(int)w)/2,100); tft.print(F("FLUSH"));
  tft.getTextBounds("URINAL",0,0,&x1,&y1,&w,&h);
  tft.setCursor((280-(int)w)/2,140); tft.print(F("URINAL"));
  tft.setFont(&FreeSansBold9pt7b); tft.setTextColor(TEXT_SECONDARY);
  tft.getTextBounds("Please flush to clean",0,0,&x1,&y1,&w,&h);
  tft.setCursor((280-(int)w)/2,180); tft.print(F("Please flush to clean"));
}

// ═════════════════════════════════════════════════════════════
//  SECTION 14 — READY SCREEN
// ═════════════════════════════════════════════════════════════
void drawReadyScreen(){
  tft.fillScreen(BG_COLOR); tft.setTextSize(1);
  int16_t x1,y1; uint16_t w,h;
  tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_PRIMARY);
  tft.getTextBounds(g_timeStr,0,0,&x1,&y1,&w,&h);
  tft.setCursor(244-(int)w,20); tft.print(g_timeStr);
  tft.setFont(&FreeSansBold18pt7b); tft.setTextColor(STATUS_GREEN);
  tft.getTextBounds("READY",0,0,&x1,&y1,&w,&h);
  tft.setCursor((280-(int)w)/2,120); tft.print(F("READY"));
  tft.setFont(&FreeSansBold9pt7b); tft.setTextColor(TEXT_SECONDARY);
  tft.getTextBounds("System ready for next user",0,0,&x1,&y1,&w,&h);
  tft.setCursor((280-(int)w)/2,170); tft.print(F("System ready for next user"));
}

// ═════════════════════════════════════════════════════════════
//  SECTION 15 — BOOT SPLASH HELPER
// ═════════════════════════════════════════════════════════════
void splashMsg(const __FlashStringHelper* msg){
  tft.fillRect(0,120,280,50,BG_COLOR);
  tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_SECONDARY);
  int16_t x1,y1; uint16_t w,h;
  tft.getTextBounds(msg,0,0,&x1,&y1,&w,&h);
  tft.setCursor((280-(int)w)/2,148); tft.print(msg);
}

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════
void setup(){
  Serial.begin(115200);
  SIM800.begin(9600);
  SPI.begin();

  pinMode(BUZZER,OUTPUT);
  digitalWrite(BUZZER,LOW);

  tft.init(240,280);
  tft.setRotation(3);
  tft.fillScreen(BG_COLOR);
  tft.setTextWrap(false);

  int16_t x1,y1; uint16_t w,h;
  tft.setFont(&FreeSansBold18pt7b);
  tft.getTextBounds("UrineScope",0,0,&x1,&y1,&w,&h);
  tft.setTextColor(ACCENT_BLUE);
  tft.setCursor((280-(int)w)/2,80); tft.print(F("UrineScope"));
  tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_SECONDARY);
  tft.getTextBounds("Smart Passive Urinalysis",0,0,&x1,&y1,&w,&h);
  tft.setCursor((280-(int)w)/2,105); tft.print(F("Smart Passive Urinalysis"));

  splashMsg(F("Initializing sensors..."));
  if(!tcs.begin()){
    tft.setFont(); tft.setTextSize(1); tft.setTextColor(STATUS_RED);
    tft.setCursor(10,10); tft.print(F("TCS34725 NOT FOUND - CHECK WIRING"));
    delay(3000);
  }
  pinMode(TDS_PIN,  INPUT);
  pinMode(MQ135_PIN,INPUT);
  pinMode(MQ3_PIN,  INPUT);

  splashMsg(F("MQ sensor warm-up (20s)..."));
  unsigned long wu=millis();
  tft.drawRect(14,162,252,8,TEXT_SECONDARY);
  while(millis()-wu<20000){
    tickRTC();
    tft.fillRect(15,163,(uint16_t)((millis()-wu)/79),6,ACCENT_BLUE);
    delay(100);
  }
  calibrateMQ();

  splashMsg(F("Syncing time via SIM800L..."));
  sendAT(F("AT"),2000);
  syncTime();
  lastRtcTick=millis();

  splashMsg(F("Fetching weather data..."));
  fetchWeather();
  lastWxFetch=millis();

  splashMsg(F("Ready!"));
  delay(600);

  drawStandbyBase();
  resetStandbyCache();
}

// ═════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═════════════════════════════════════════════════════════════
void loop(){

  tickRTC();

  if(sysState==STANDBY&&millis()-lastWxFetch>=WX_INTERVAL){
    lastWxFetch=millis();
    fetchWeather();
  }

  switch(sysState){

    case STANDBY:{
      if(!standbyBaseDrawn){ drawStandbyBase(); resetStandbyCache(); }
      float tds=readTDS();
      updateStandby(tds);
      if(tds>TDS_DETECT_PPM){
        if(tdsAboveAt==0) tdsAboveAt=millis();
        if(millis()-tdsAboveAt>=TDS_SUSTAIN_MS){
          tdsAboveAt=0;
          sysState=SETTLING; stateAt=millis();
          standbyBaseDrawn=false;
          drawAnalysingScreen();
        }
      } else { tdsAboveAt=0; }
      break;
    }

    case SETTLING:{
      static uint8_t dotCount=0;
      static unsigned long lastDot=0;
      if(millis()-lastDot>500){
        lastDot=millis();
        tft.setFont(); tft.setTextSize(2);
        tft.fillRect(60,160,160,22,BG_COLOR);
        tft.setTextColor(ACCENT_BLUE);
        tft.setCursor(60,163);
        for(uint8_t i=0;i<=dotCount%4;i++) tft.print(F(". "));
        dotCount++;
      }
      if(millis()-stateAt>=SETTLING_MS){
        sysState=SAMPLING; stateAt=millis();
      }
      break;
    }

    case SAMPLING:{
      tft.drawRect(6,170,268,10,TEXT_SECONDARY);
      float sumR=0,sumG=0,sumB=0,sumMQ135=0,sumMQ3=0,peakTDS=0;
      const uint8_t N=15;
      for(uint8_t i=0;i<N;i++){
        tickRTC();
        float t=readTDS();
        if(t>peakTDS) peakTDS=t;
        float rf,gf,bf; tcs.getRGB(&rf,&gf,&bf);
        sumR+=rf; sumG+=gf; sumB+=bf;
        sumMQ135+=getMQRatio(MQ135_PIN,mq135_R0);
        sumMQ3  +=getMQRatio(MQ3_PIN,  mq3_R0);
        tft.fillRect(7,171,(uint16_t)((i+1)*266.0f/N),8,ACCENT_BLUE);
        delay(SAMPLE_INTERVAL);
      }
      uint8_t R=(uint8_t)(sumR/N),G=(uint8_t)(sumG/N),B=(uint8_t)(sumB/N);
      float r135=sumMQ135/N, r3=sumMQ3/N;
      lastDx=runDiagnostics(peakTDS,R,G,B,r135,r3);
      playResultBuzzer(lastDx);
      drawResultsScreen(lastDx);
      postToFirebase(lastDx);
      sysState=DISPLAYING; stateAt=millis(); screenDrawn=true;
      break;
    }

    case DISPLAYING:{
      static int8_t dispSec=-1;
      if(rtc_s!=dispSec){
        tft.fillRect(115,2,160,24,BG_COLOR);
        tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_PRIMARY);
        int16_t x1,y1; uint16_t w,h;
        tft.getTextBounds(g_timeStr,0,0,&x1,&y1,&w,&h);
        tft.setCursor(244-(int)w,20); tft.print(g_timeStr);
        dispSec=rtc_s;
      }
      if(millis()-stateAt>=DISPLAY_MS){
        drawFlushScreen();
        sysState=FLUSHING; stateAt=millis(); screenDrawn=true;
      }
      break;
    }

    case FLUSHING:{
      static int8_t flushSec=-1;
      if(rtc_s!=flushSec){
        tft.fillRect(115,2,160,24,BG_COLOR);
        tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_PRIMARY);
        int16_t x1,y1; uint16_t w,h;
        tft.getTextBounds(g_timeStr,0,0,&x1,&y1,&w,&h);
        tft.setCursor(244-(int)w,20); tft.print(g_timeStr);
        flushSec=rtc_s;
      }
      if(millis()-stateAt>=FLUSH_MS && readTDS() < TDS_DETECT_PPM){
        drawReadyScreen();
        buzzerReady();
        sysState=READY; stateAt=millis(); screenDrawn=true;
      }
      break;
    }

    case READY:{
      static int8_t readySec=-1;
      if(rtc_s!=readySec){
        tft.fillRect(115,2,160,24,BG_COLOR);
        tft.setFont(&FreeSans9pt7b); tft.setTextColor(TEXT_PRIMARY);
        int16_t x1,y1; uint16_t w,h;
        tft.getTextBounds(g_timeStr,0,0,&x1,&y1,&w,&h);
        tft.setCursor(244-(int)w,20); tft.print(g_timeStr);
        readySec=rtc_s;
      }
      if(millis()-stateAt>=READY_MS){
        sysState=STANDBY;
        standbyBaseDrawn=false;
        tdsAboveAt=0;
      }
      break;
    }
  }
}
