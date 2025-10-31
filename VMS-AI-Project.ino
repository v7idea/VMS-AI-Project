#include <Arduino.h>
#include <stdint.h> // For uint8_t

#define U8G2_USE_LARGE_FONTS true

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <WiFi.h>
#include "Base64.h"
#include "VideoStream.h"
#include "audio_api.h"
#include "opus.h" // 確保包含 Opus 頭文件
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include "AIAssistant.h"
#include <vector>        // 用於 std::vector 來追蹤唯一 SSID (或者用 std::set 效率更高)
#include "wifi_html.h"
#include "wifi_flash.h"
#include "v7_tft_lcd.h"
#include "v7lcdfont.h"

// Camera definnitation
#define VIDEO_CHANNEL 0
VideoSetting config(640, 480, CAM_FPS, VIDEO_JPEG, 1);
uint32_t img_addr = 0;
uint32_t img_len = 0;

#define math_min(a, b) ((a) < (b) ? (a) : (b))

#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// char ssid[] = "";    // your network SSID (name)
// char ssid[] = "";    // your network SSID (name)
// char pass[] = "";        // your network password
int status = WL_IDLE_STATUS;     // Indicator of Wifi status
//byte mac[6];
uint8_t mac[6];

char temp_ssid[37]; // 假設 SSID 最長 63 字元 + null
char temp_pass[64]; // 假設 SSID 最長 63 字元 + null

// --- 全局變數 ---
JsonDocument wifiListJson; // 全局 JSON 文檔，用於存儲掃描到的 Wi-Fi SSID 
unsigned long lastScanTime = 0;
const unsigned long scanInterval = 15000; // 15 秒
bool isAPmode = false;
WiFiServer server(80);  // 執行http Server

// 傳送image, 暫時先放在這邊，之後再移到其他地方

bool isSendingImage = false;

void sendImageToMQTT() {

    if(isSendingImage) {
        return;
    }
    isSendingImage = true;

    Camera.getImage(VIDEO_CHANNEL, &img_addr, &img_len);

    Serial.print("[NOTE]:capture photo, size:");
    Serial.println((int)img_len);

    int max_packet_size = MQTT_MAX_PACKET_SIZE;

    client.beginPublish(publishPhotoTopic, img_len, false);

    for (int i = 0; i < (int)img_len; i += max_packet_size) {
        int chunk_size = math_min(max_packet_size, (int)img_len - i);
        client.write((uint8_t*)img_addr + i, chunk_size);
    }

    bool isPublished = client.endPublish();

    if (isPublished) {
        Serial.println("Publish image to MQTT server successful");
    } else {
        Serial.println("Publish image to MQTT server failed");
    }

    isSendingImage = false;

}


// End of send image to mqtt server;


struct WiFiCredentials {
  char ssid[33];
  char password[65];
  char assistantCharactor[512];
  char voiceType[20];
  bool configured;
};

WiFiCredentials credentials;

char apSSID[33] = "VMS_AI_";    // Set the AP SSID
char apPASSWORD[65] = "12345678";        // Set the AP password
char channel[] = "5";               // Set the AP channel
char macStr[12];
int ssid_status = 0;                // Set SSID status, 1 hidden, 0 not hidden
// 用於演示的全局變數
String post_ssid = "";
String post_password = "";

char mqttServer[] = "220.128.96.93";
char clientId[12];
char clientUser[] = "xiaozhi";
char clientPass[] = "xiaozhi";
char publishPayload[] = "Hello, test!";
// char publishTopic[] = "robot/1234/command";
// char subscribeTopic[] = "robot/1234/message";
// char subscribeAudioTopic[] = "robot/1234/voiceFeedback";

char publishTopic[100];
char publishPhotoTopic[100];
char subscribeTopic[100];
char subscribeAudioTopic[100];

int mqttPort = 11883;

int opus_encode_packet_count = 0;
int opus_decode_packet_count = 0;
int mqtt_sent_packet_count = 0;
unsigned long last_stats_time = 0;
int last_encoded_len = 0;                       // 用於統計
int last_decoded_samples = 0;                   // 用於統計
long long last_frame_energy_for_debug = 0;      // 用於調試時打印能量

int volIndex = 5;
uint volSteps[] = {0x5F, 0x68, 0x6F, 0x78, 0x7F, 0x88, 0x8F, 0x98, 0x9F, 0xA8, 0xAF};

uint dacVol = volSteps[volIndex];
bool dacDriection = false;

// TFT LCD
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

void initWiFi()
{

    int detect_wifi_count = 3;

    String connectMessage = String("連線中..\n") + temp_ssid;
    const char* messageCStr = connectMessage.c_str();

    // Attempt to connect to WiFi network
    while (status != WL_CONNECTED && detect_wifi_count > 0) {

        // u8g2.clearBuffer();
        topMenuArea(u8g2, "WIFI", 0, 0);
        drawAutoWrappedText(u8g2, 20, 6, 108, messageCStr);
        // u8g2.sendBuffer();
        // topMenuArea(u8g2, messageCStr, 0, 0);

        detect_wifi_count --;
        Serial.print("\r\nAttempting to connect to SSID: ");
        Serial.println(temp_ssid);
        // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
        status = WiFi.begin(temp_ssid, temp_pass);
        // wait 10 seconds for connection:
        delay(10000);

    }

    if(status != WL_CONNECTED) {        // 如果一直沒有連線成功，就需要重新啟動AP模式

        // u8g2.clearBuffer();
        // drawAutoWrappedText(u8g2, 20, 32, 108, "無法連上WIFI");
        topMenuArea(u8g2, "無法連上WIFI", 0, 0);
        // u8g2.sendBuffer();

        WiFi.enableConcurrent();
        // 進入WIFI AP Server設定
        digitalWrite(WIFIAP_LED, HIGH);
        wifiAPProcess();
        printMacAddress();
        isAPmode = true;

        return;

    }

    // getWifiMac();
    // 取得 MAC Address
    
    WiFi.macAddress(mac);  // MAC address 會填入 mac 陣列中

    snprintf(clientId, sizeof(clientId), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 格式化為字串 "/robot/XX:XX:XX:XX:XX:XX/message"
    snprintf(publishTopic, sizeof(publishTopic), "robot/%02X%02X%02X%02X%02X%02X/command",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 格式化為字串 "/robot/XX:XX:XX:XX:XX:XX/message"
    snprintf(subscribeTopic, sizeof(subscribeTopic), "robot/%02X%02X%02X%02X%02X%02X/message",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 格式化為字串 "/robot/XX:XX:XX:XX:XX:XX/message"
    snprintf(subscribeAudioTopic, sizeof(subscribeAudioTopic), "robot/%02X%02X%02X%02X%02X%02X/voiceFeedback",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    //publishPhotoTopic
    snprintf(publishPhotoTopic, sizeof(publishPhotoTopic), "robot/%02X%02X%02X%02X%02X%02X/photoCommand",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    digitalWrite(WIFICLIENT_LED, HIGH);

    wifiClient.setNonBlockingMode();
    client.setServer(mqttServer, mqttPort);
    client.setCallback(callback);

    // u8g2.clearBuffer();
    // drawAutoWrappedText(u8g2, 20, 32, 108, "主機連線中..");
    topMenuArea(u8g2, "連線到主機", 0, 0);
    drawAutoWrappedText(u8g2, 20, 12, 108, "");
    // u8g2.sendBuffer();

    // Allow Hardware to sort itself out
    delay(1500);
}

void printMacAddress()
{
    // print your MAC address:
    byte mac[6];
    WiFi.BSSID(mac);
    Serial.print("MAC: ");
    Serial.print(mac[0], HEX);
    Serial.print(":");
    Serial.print(mac[1], HEX);
    Serial.print(":");
    Serial.print(mac[2], HEX);
    Serial.print(":");
    Serial.print(mac[3], HEX);
    Serial.print(":");
    Serial.print(mac[4], HEX);
    Serial.print(":");
    Serial.println(mac[5], HEX);

    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
           
}

char statusText[100];
char voiceText[100];
int16_t statusTextX;       // 起始 x 座標
int16_t voiceTextX;
uint16_t statusTextWidth;  // 文字寬度（用於循環）
uint16_t voiceTextWidth;  // 文字寬度（用於循環）

const char* apInitAlert = "SSID:VMS_AI_****\n密碼: 12345678\nhttp://192.168.1.1";

const char* apModeText = "WIFI AP模式";

int apInitAlertY = 14;
// 一開始設定啟動

int lightGPIO = AMB_D16;
int fanGPIO = AMB_D20;
int voiceMute_PIN = AMB_D17;

void setup() {

    Serial.begin(115200);
    while (!Serial);

    u8g2.begin();
    u8g2.enableUTF8Print();		// exnable UTF8 support for the Arduino print() function
    u8g2.setFont(v7idea_font_wqy12_chinese);  // use chinese2 for all the glyphs of "你好世界"

    u8g2.clearBuffer();
    String str = "系統啟動中..";
    str.toCharArray(statusText, sizeof(statusText));
    // statusTextWidth = u8g2.getStrWidth(statusText);  // 計算整段文字寬度
    // statusTextX = (u8g2.getDisplayWidth() - statusTextWidth) / 2;  // 從右邊畫面外開始

    u8g2.setCursor(20, 40);
    u8g2.print(statusText);
    u8g2.sendBuffer();

    pinMode(SWITCH_BUTTON,  INPUT_PULLUP);
    pinMode(VOL_UP_BUTTON,  INPUT_PULLUP);
    pinMode(VOL_DOWN_BUTTON,  INPUT_PULLUP);
    pinMode(WIFIAP_LED,     OUTPUT);
    pinMode(WIFICLIENT_LED, OUTPUT);
    pinMode(lightGPIO, OUTPUT);
    pinMode(fanGPIO, OUTPUT);
    pinMode(voiceMute_PIN, OUTPUT);

    digitalWrite(lightGPIO, HIGH);
    digitalWrite(fanGPIO, HIGH);
    digitalWrite(voiceMute_PIN, LOW);

    // Print WiFi MAC address:
    printMacAddress();
    strcat(apSSID, macStr); // 再將 str2 附加到 result

    Serial.println("");
    Serial.println("檢查Flash設定");

    FlashMemory.begin(WIFI_CONFIG_FLASH_ADDR, sizeof(WifiCredentials));
    // FlashMemory.begin(FLASH_MEMORY_APP_BASE, sizeof(WifiCredentials));
    
    if (FlashMemory.buf == NULL) {
        Serial.println("錯誤: 無法正確啟動FlashMemory Buffer, 有事先Locate?");
        while(1);
    }
    FlashMemory.end();

    Serial.println("FlashMemory啟動成功. Buffer size: " + String(FlashMemory.buf_size) + " bytes.");
    readWifiCredentials(temp_ssid, sizeof(temp_ssid), temp_pass, sizeof(temp_pass));

    str = "檢查Flash設定完成";
    str.toCharArray(statusText, sizeof(statusText));
    statusTextWidth = u8g2.getStrWidth(statusText);  // 計算整段文字寬度
    statusTextX = (u8g2.getDisplayWidth() - statusTextWidth) / 2;  // 從右邊畫面外開始
    
    u8g2.clearBuffer();
    u8g2.setCursor(10, 40);
    u8g2.print(statusText);
    u8g2.sendBuffer();

    // if(!digitalRead(SWITCH_BUTTON) || temp_ssid[0] == '\0') {
    if(!digitalRead(SWITCH_BUTTON) || temp_ssid[0] == '\0') {
        // 進入WIFI AP Server設定
        digitalWrite(WIFIAP_LED, HIGH);
        wifiAPProcess();
        printMacAddress();
        isAPmode = true;

        // u8g2.clearBuffer();
        topMenuArea(u8g2, apModeText, 0, 0);
        drawAutoWrappedText(u8g2, 0, 0, 108, apInitAlert, -1);
        // u8g2.sendBuffer();

    } else {

        // u8g2.clearBuffer();
        // drawAutoWrappedText(u8g2, 20, 32, 108, "WIFI連線中");
        // u8g2.sendBuffer();
        topMenuArea(u8g2, "連線中", 0, 0);

        delay(1000);

        // 進入WIFI Client設定
        // WiFi.begin(temp_ssid, temp_pass);
        // int retries = 0;
        // const int maxRetries = 30; // 大約 15 秒 (30 * 500ms)
        // while (WiFi.status() != WL_CONNECTED && retries < maxRetries) {
        //     delay(500);
        //     Serial.print(".");
        //     retries++;
        // }

        // if (WiFi.status() == WL_CONNECTED) {
        //     digitalWrite(WIFICLIENT_LED, HIGH);
        //     Serial.println("WIFI已經連線");
        // }

        currentRobotState = ROBOT_CONNECT_WIFI;
        initWiFi(); // 設定Wifi;

        Serial.println("系统设置开始...");
        currentRobotState = ROBOT_WAIT_SND_MODULE_INIT;

        setup_audio_mic_input();  // 設定 audio devices;
        setup_opus_codec(); // 初始化 Opus 編碼器
        setup_vad(); // ** 調用 VAD 初始化 **
        
        Serial.println("初始化完成。");
        
        // u8g2.clearBuffer();
        // drawAutoWrappedText(u8g2, 20, 32, 108, "一切就緒！");
        // u8g2.sendBuffer();
        topMenuArea(u8g2, "一切就緒！", 0, 0);

        audio_dac_digital_vol(&audio_dev, dacVol);

    }

    // 增加攝像頭的相關處理，啟動鏡頭的處理;
    // Camera.configVideoChannel(VIDEO_CHANNEL, config);
    // Camera.videoInit();
    // Camera.channelBegin(VIDEO_CHANNEL);
    // config.setRotation(0);

    // 啟動鏡頭
    config._use_static_addr = 0;
    Camera.configVideoChannel(VIDEO_CHANNEL, config);
    delay(1000);
    Camera.videoInit(VIDEO_CHANNEL);
    delay(1000);
    Camera.channelBegin(VIDEO_CHANNEL);

    // // 嘗試讀取最後 4KB 看是否已有資料
    FlashMemory.begin(WIFI_CONFIG_FLASH_ADDR, sizeof(WifiCredentials));
    unsigned int value;

    for (int i = 0; i < sizeof(WifiCredentials); i += 4) {
        // Serial.print("0x%08X: %08X\n", 0xFF000 + i, FlashMemory.readWord(0xFF000 + i));
        Serial.print("0x");
        Serial.print(WIFI_CONFIG_FLASH_ADDR + i, HEX);
        Serial.print(": ");
        value = FlashMemory.readWord(i);
        Serial.println(value, HEX);
    }
    FlashMemory.end();

    // delay(5000);    


}

void loop() {


    if(isAPmode) {

        drawAutoWrappedText(u8g2, 0, 0, 108, apInitAlert, -1);
        topMenuArea(u8g2, apModeText, 0, 0);

        WiFiClient client = server.available();    // listen for incoming clients
        handleWIFIAPWebProcess(client);
        delay(200);
        
        return;

    } 

    if (!(client.connected())) {

        // u8g2.clearBuffer();
        // drawAutoWrappedText(u8g2, 10, 32, 108, "失去與主機連線...");
        topMenuArea(u8g2, "失去連線!", 0, 0);
        // u8g2.sendBuffer();

        isRobotReady = false;
        isRobotListen = false;
        reconnectMQTT();  
    }

    // 處理麥克風數據的 Opus 編碼和 MQTT 發送
    if (new_audio_data_ready_for_opus_encoding && !isTTSProcess && currentRobotState != ROBOT_IDLE) {   // 必須要在沒有處理TTS才接收
        
        new_audio_data_ready_for_opus_encoding = false;
        uint8_t* local_raw_buffer_ptr = (uint8_t*)dma_filled_rx_buffer_ptr;
        const opus_int16* pcm_samples_full_frame = (const opus_int16*)local_raw_buffer_ptr;
 
        // --- VAD (能量檢測) 處理 ---
        // bool current_frame_has_voice = false;
        // long long current_frame_total_energy = 0;

        // for (int i = 0; i < FRAME_SIZE_SAMPLES; ++i) {
        //     // 直接使用樣本值，因為 opus_int16 (通常是 short) 的平方不會輕易溢出 long long
        //     current_frame_total_energy += (long long)pcm_samples_full_frame[i] * pcm_samples_full_frame[i];
        // }
        // last_frame_energy_for_debug = current_frame_total_energy; // 保存能量值用於調試打印

        // if (current_frame_total_energy > VAD_ENERGY_THRESHOLD) {
        //     current_frame_has_voice = true;
        // }
        // // --- VAD 狀態保持 (Hangover) ---
        // if (current_frame_has_voice) {
        //     vad_voice_active_flag = true;
        //     vad_last_voice_time_ms = millis();
        // } else {
        //     if (vad_voice_active_flag && (millis() - vad_last_voice_time_ms < VAD_HANGOVER_DURATION_MS)) {
        //         // 處於 hangover 狀態，仍然認為有語音
        //     } else {
        //         vad_voice_active_flag = false;
        //     }
        // }
        // -----------------------------

        vad_voice_active_flag = true;

        if (vad_voice_active_flag) { // ** 根據 VAD 結果決定是否編碼和發送 **
            opus_encode_packet_count++;
            last_encoded_len = opus_encode(encoder, pcm_samples_full_frame, FRAME_SIZE_SAMPLES, opus_encoded_buffer, MAX_ENCODED_PACKET_SIZE);

            if (last_encoded_len > 0) {
                if (client.connected()) {
                    bool published = client.publish(publishTopic, opus_encoded_buffer, last_encoded_len);
                    if (published) {
                        mqtt_sent_packet_count++;
                    } else {
                        Serial.println("MQTT: Publish failed (voice)!");
                    }
                }
            } else if (last_encoded_len < 0) {
                Serial.print("Opus 编码失败: "); Serial.println(opus_strerror(last_encoded_len));
            }
        } else {
            // 當沒有語音時，可以選擇什麼都不做，或者發送一個特殊的靜音包/心跳包（如果需要）
            last_encoded_len = 0; // 表示本幀未編碼
        }

    }

    // 處理從 MQTT 收到的 Opus 數據的解碼並放入播放緩衝區
    if (new_opus_data_received_for_decoding) {
        last_decoded_samples = opus_decode(decoder, (const unsigned char*)mqtt_received_opus_payload, mqtt_received_opus_length, pcm_decoded_buffer, FRAME_SIZE_SAMPLES, 0);

        if (last_decoded_samples < 0) {
            Serial.print("Opus 解码失败: "); Serial.println(opus_strerror(last_decoded_samples));
        } else {

            opus_decode_packet_count++;     
            if (last_decoded_samples == FRAME_SIZE_SAMPLES) {
                // 嘗試將解碼後的 PCM 數據放入環形緩衝區
                // 臨界區保護
                noInterrupts();
                while(playback_frames_in_buffer >= PLAYBACK_RING_BUFFER_FRAMES) {
                    delay(5);
                    //Serial.println("DECODE Loop: delay 10ms to wait the playback ring buffer!!");
                }
                if (playback_frames_in_buffer < PLAYBACK_RING_BUFFER_FRAMES) {
                    memcpy(playback_ring_buffer[playback_write_idx], pcm_decoded_buffer, PCM_BUFFER_SIZE_BYTES);
                    playback_write_idx = (playback_write_idx + 1) % PLAYBACK_RING_BUFFER_FRAMES;
                    playback_frames_in_buffer++;
                    // Serial.print("Loop: Decoded PCM added to RB. Buf count: "); Serial.println(playback_frames_in_buffer);
                } else {
                    Serial.println("Loop: Playback ring buffer overflow! Dropping decoded frame.");
                }
                interrupts();
            } else {
                Serial.print("Opus 解码樣本數與期望不符: "); 
                Serial.print(last_decoded_samples); 
                Serial.println(FRAME_SIZE_SAMPLES);
            }
        }

        new_opus_data_received_for_decoding = false; // 清除輸入標誌
    }

    if(DEBUG_MODE) {
       // ... (統計信息打印邏輯保持不變) ...
      unsigned long current_time = millis();
      if (current_time - last_stats_time >= 1000) {
          last_stats_time = current_time;
          Serial.print("Enc/s: "); Serial.print(opus_encode_packet_count);
          Serial.print(", Dec/s: "); Serial.print(opus_decode_packet_count);
          Serial.print(", MQTT tx/s: "); Serial.println(mqtt_sent_packet_count);
          Serial.print("  Playback Buf: "); Serial.print(playback_frames_in_buffer);
          Serial.print(", VAD active: "); Serial.println(vad_voice_active_flag ? "YES" : "NO");
          if (vad_debug_print_energy) { // 如果啟用調試打印
              Serial.print("  Last Frame Energy: "); Serial.println(last_frame_energy_for_debug);
          }
          if (mqtt_sent_packet_count > 0 && last_encoded_len > 0) { Serial.print("  Last Enc Pkt (bytes): "); Serial.println(last_encoded_len); }
          if (opus_decode_packet_count > 0 && last_decoded_samples > 0) { Serial.print("  Last Dec Samples: "); Serial.println(last_decoded_samples * CHANNELS); }
          opus_encode_packet_count = 0; 
          opus_decode_packet_count = 0; 
          mqtt_sent_packet_count = 0;
      }
    }

    if(!digitalRead(SWITCH_BUTTON)) {

        // u8g2.clearBuffer();
        // drawAutoWrappedText(u8g2, 20, 32, 108, "功能鍵...");
        // u8g2.sendBuffer();

        if(currentRobotState != ROBOT_IDLE) {

            currentRobotState = ROBOT_IDLE;
            // u8g2.clearBuffer();
            // drawAutoWrappedText(u8g2, 20, 32, 108, "待機模式！");
            // u8g2.sendBuffer();
            topMenuArea(u8g2, "待機模式！", 0, 0);
            drawAutoWrappedText(u8g2, 0, 12, 128, "-來歇一下-");

            // 需要傳送 about 訊息到主機端
            char reason[] = "click function key to about TTS/STT";
            sendAbortToServer(reason);  // 發送about到主機
            isTTSProcess = false;
            digitalWrite(WIFIAP_LED, LOW);
            delay(1000);

        } else {
            currentRobotState = ROBOT_LISTEN;
            // u8g2.clearBuffer();
            // drawAutoWrappedText(u8g2, 20, 32, 108, "聆聽中...");
            // u8g2.sendBuffer();

            topMenuArea(u8g2, "聆聽中", 0, 0);
            drawAutoWrappedText(u8g2, 20, 12, 108, "");

            sendHelloToServer();
            isTTSProcess = false;

            delay(1000);
        }
    }

    if(!digitalRead(VOL_UP_BUTTON) && !digitalRead(VOL_DOWN_BUTTON)) {

        // u8g2.clearBuffer();
        // drawAutoWrappedText(u8g2, 20, 32, 108, "傳送照片");
        // u8g2.sendBuffer();
        topMenuArea(u8g2, "傳送照片", 0, 0);
         // 同時按下兩個聲音鍵;
        sendImageToMQTT();
        delay(2000);

       
    } else {

        // 如果只有按下單一按鈕的處理

        if(!digitalRead(VOL_UP_BUTTON)) {

            if(volIndex < 10) {
                volIndex ++;
                dacVol = volSteps[volIndex];
            }

            Serial.print("聲音變大, volIndex:");
            Serial.println(volIndex);
            Serial.print("聲音變大, dacVol:");
            Serial.println(dacVol);



            // u8g2.clearBuffer();
            //drawAutoWrappedText(u8g2, 20, 32, 108, "聲音變大...");
            // u8g2.sendBuffer();
            topMenuArea(u8g2, "音量 +", 0, 0);
            audio_dac_digital_vol(&audio_dev, dacVol);
            delay(500);

        }

        if(!digitalRead(VOL_DOWN_BUTTON)) {

            // if(dacVol >= 5) {
            //     dacVol -= 5;
            //     Serial.println("vol -");
            // }

            // if(dacVol < 5) {
            //     dacVol = 0;
            // }

            if(volIndex > 0) {
                volIndex = volIndex - 1;
                dacVol = volSteps[volIndex];
            }

            Serial.print("聲音變小, volIndex:");
            Serial.println(volIndex);
            Serial.print("聲音變小, dacVol:");
            Serial.println(dacVol);

            audio_dac_digital_vol(&audio_dev, dacVol);
            // u8g2.clearBuffer();
            // drawAutoWrappedText(u8g2, 20, 32, 108, "聲音變小...");
            // u8g2.sendBuffer();
            topMenuArea(u8g2, "音量 -", 0, 0);
            delay(500);

        }


    }

    client.loop();

    // if(!digitalRead(SWITCH_BUTTON) && digitalRead(VOL_UP_BUTTON)) {
    //     // 兩個同時按，表示取消語音;
    //     Serial.println("取消聲音指令");

    // } else if(!digitalRead(SWITCH_BUTTON) && !digitalRead(VOL_UP_BUTTON)) {
    //     // 表示聲音變小
    //     if(!dacDriection) {
    //         if(dacVol > 0) {
    //             dacVol--;
    //             Serial.println("vol -");
    //         } else {
    //             dacDriection = true;
    //         }
            
    //     } else {

    //         if(dacVol < DVOL_DAC_0DB) {
    //             dacVol++;
    //             Serial.println("vol +");
    //         } else {
    //             dacDriection = false;
    //         }

    //     }
    //     audio_dac_digital_vol(&audio_dev, dacVol);
    //     delay(5);
    // } else if(digitalRead(SWITCH_BUTTON) && !digitalRead(VOL_UP_BUTTON)) {
    //     // 表示聲音變大
    //     if(dacVol < DVOL_DAC_0DB) {
    //         dacVol ++;
    //     }
    //     audio_dac_digital_vol(&audio_dev, dacVol);
    //     Serial.println("vol +");
    //     delay(20);
        
    // }

    
    delay(2);
   
}