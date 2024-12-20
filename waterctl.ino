#include <BLEDevice.h>
#include <Arduino.h>
#include <time.h>
#include <sstream>
#include <iomanip>

#include <wasm3.h>

#include "dino.wasm.h"


#define SERVICE_UUID "F1F0"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

#define CHARACTERISTIC_TXD_UUID "F1F1"
#define CHARACTERISTIC_RXD_UUID "F1F2"


#define FATAL(func, msg) { Serial.print("Fatal: " func " "); Serial.println(msg); while(1) { delay(100); } }

boolean doSacn = true;
boolean doConnect = false;
boolean connected = false;

BLEAdvertisedDevice* pServer;
BLERemoteCharacteristic* pRemoteCharacteristic;
BLERemoteCharacteristic* txdCharacteristic;
BLERemoteCharacteristic* rxdCharacteristic;
BLEClient* pClient;

char* hexToString(const unsigned char* array, size_t length) {
  char* hexString = (char*)malloc(length * 2 + 1);

  for (size_t i = 0; i < length; ++i) {
    sprintf(hexString + i * 2, "%02X", array[i]);
  }

  hexString[length * 2] = '\0';
  return hexString;
}

// 搜索到设备时回调功能
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    //Serial.printf("Advertised Device: %s \n", advertisedDevice.toString().c_str()); // 打印设备信息
    //Serial.printf("Advertised Device: %s %d \n" , advertisedDevice.getAddress().toString().c_str() , (String(advertisedDevice.getAddress().toString().c_str())=="6d:6c:00:02:73:63")); // 打印设备信息
    // if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(BLEUUID("4fafc201-1fb5-459e-8fcc-c5c9c331914b"))) {
    if (String(advertisedDevice.getAddress().toString().c_str()) == "6d:6c:00:02:73:63") {
      advertisedDevice.getScan()->stop();                   // 停止当前扫描
      pServer = new BLEAdvertisedDevice(advertisedDevice);  // 暂存设备
      doSacn = false;
      doConnect = true;
      Serial.println("发现想要连接的设备");
    }
  }
};

// 客户端与服务器连接与断开回调功能
class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    doSacn = false;
    connected = true;
    Serial.println("已建立与设备的连接");
  }
  void onDisconnect(BLEClient* pclient) {
    doSacn = true;
    connected = false;
    Serial.println("失去与设备的连接");
  }
};

uint16_t crc16changgong(const char* str) {
  uint16_t crc = 0x1017;  // 初始值，根据算法要求设置
  uint8_t data;
  uint8_t temp;
  int i, j;

  while (*str) {
    data = (uint8_t)*str++;
    crc ^= data;

    for (j = 0; j < 8; j++) {
      temp = crc & 0x0001;  // 取出最低位
      if (temp) {
        crc >>= 1;
        crc ^= 0xa001;  // 多项式0x8005的位翻转形式
      } else {
        crc >>= 1;
      }
    }
  }

  return crc;
}

// 辅助函数：生成随机用户ID（2字节）
uint16_t makeRandomUserId() {
  return random(0xFFFF);  // 生成一个0到65535之间的随机数
}

// 获取当前日期和时间的函数，返回一个std::vector<uint8_t>
std::vector<uint8_t> makeDatetimeArray() {
  // 获取当前时间，设置为UTC时区，因为localtime_r需要time_t输入
  time_t now = time(nullptr);
  struct tm* utc_time = gmtime_r(&now, nullptr);

  // 由于gmtime_r返回的是UTC时间，我们需要将其转换为北京时间（UTC+8）
  utc_time->tm_hour += 8;
  if (utc_time->tm_hour >= 24) {
    utc_time->tm_hour -= 24;
    utc_time->tm_mday += 1;
    // 检查是否需要调整月份和年份（这里省略了闰年和平年的判断，仅作示例）
    if (utc_time->tm_mday > 31) {
      // 简单的月份天数判断，不处理所有情况
      utc_time->tm_mday = 1;
      utc_time->tm_mon += 1;
      if (utc_time->tm_mon > 11) {
        utc_time->tm_mon = 0;
        utc_time->tm_year += 1;
      }
    }
  }

  // 提取日期和时间信息
  int year = utc_time->tm_year + 1900;  // 年份从1900年开始计数
  int month = utc_time->tm_mon + 1;     // 月份从0开始计数，需要加1
  int day = utc_time->tm_mday;
  int hour = utc_time->tm_hour;
  int minute = utc_time->tm_min;
  int second = utc_time->tm_sec;

  // 构造日期时间数组（年份取后两位，以符合示例格式）
  std::vector<uint8_t> datetimeArray = {
    static_cast<uint8_t>(year % 100),
    static_cast<uint8_t>(month),
    static_cast<uint8_t>(day),
    static_cast<uint8_t>(hour),
    static_cast<uint8_t>(minute),
    static_cast<uint8_t>(second)
  };

  // 注意：这里没有使用decAsHex函数，因为static_cast<uint8_t>已经完成了转换
  // 如果你的环境或代码风格要求使用类似的函数，你可以将其添加回去

  return datetimeArray;
}

// 主函数：生成会话开始的后记（epilogue）
std::vector<uint8_t> makeStartEpilogue(const char* deviceName, bool isKeyAuthPresent = false) {
  uint16_t checksum = crc16changgong(deviceName + strlen(deviceName) - 5);  // 计算设备名最后5个字符的CRC16
  uint8_t mn = isKeyAuthPresent ? 0x0B : 0xFF;                              // 魔法数字
  uint16_t ri = makeRandomUserId();                                         // 随机用户ID
  //std::vector<uint8_t> dt = makeDatetimeArray();                            // 日期时间数组

  // 构造并返回Uint8Array（在C++中使用std::vector<uint8_t>）
  std::vector<uint8_t> epilogue = {
    0xFE, 0xFE, 0x09, 0xB2,
    0x01, static_cast<uint8_t>(checksum & 0xFF), static_cast<uint8_t>(checksum >> 8), mn,
    0x00, static_cast<uint8_t>(ri & 0xFF), static_cast<uint8_t>(ri >> 8),  // 2字节的随机用户ID
 //   dt[0], dt[1], dt[2], dt[3], dt[4], dt[5],                              // 6字节的日期时间
    0x0F, 0x27, 0x00
  };

  return epilogue;
}

std::vector<uint8_t> hexStringToVector(const std::string& hexStr) {
  std::vector<uint8_t> bytes;
  std::istringstream iss(hexStr);
  std::string byteString;
  while (iss >> std::setw(2) >> std::setfill('0') >> byteString) {
    bytes.push_back(static_cast<uint8_t>(strtol(byteString.c_str(), nullptr, 16)));
  }
  return bytes;
}

// CRC-16/CGAEAF (abbrev for ChangGong AE/AF)
// width=16 poly=0x8005 init=0xf856 refin=true refout=true xorout=0x0075
// truncated to lower 8 bits
uint8_t crc16cgaeaf(const std::vector<uint8_t>& data) {
  uint16_t crc = 0x6A1F;

  for (uint8_t byte : data) {
    crc ^= byte << 8;  // 将数据字节移到CRC的高8位位置

    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000) {           // 检查最高位
        crc = (crc << 1) ^ 0xA001;  // 左移并应用多项式
      } else {
        crc <<= 1;  // 仅左移
      }
    }
  }

  crc = (crc ^ 0x75) & 0xFF;  // 应用最终异或值并只保留低8位
  return crc;
}

IM3Environment env;
IM3Runtime runtime;
IM3Module module;
IM3Function func_run;
uint8_t* mem;

void load_wasm() {
  M3Result result = m3Err_none;

  if (!env) {
    env = m3_NewEnvironment();
    if (!env) FATAL("NewEnvironment", "failed");
  }

  m3_FreeRuntime(runtime);

  runtime = m3_NewRuntime(env, 1024, NULL);
  if (!runtime) FATAL("NewRuntime", "failed");

  result = m3_ParseModule(env, &module, dino_wasm, sizeof(dino_wasm));
  if (result) FATAL("ParseModule", result);

  result = m3_LoadModule(runtime, module);
  if (result) FATAL("LoadModule", result);

  mem = m3_GetMemory(runtime, NULL, 0);
  if (!mem) FATAL("GetMemory", "failed");

  result = m3_FindFunction(&func_run, runtime, "makeKey");
  if (result) FATAL("FindFunction", result);
}

std::vector<uint8_t> extractUint32AsBytes(uint8_t* memory, size_t offset) {
  // 计算32位整数在内存中的位置
  uint32_t* keyPtr = reinterpret_cast<uint32_t*>(memory + offset);

  // 读取32位整数
  uint32_t key = *keyPtr;

  // 将32位整数分解为四个字节
  std::vector<uint8_t> result(4);
  result[0] = (key >> 24) & 0xff;
  result[1] = (key >> 16) & 0xff;
  result[2] = (key >> 8) & 0xff;
  result[3] = key & 0xff;

  return result;
}

std::vector<uint8_t> makeUnlockKey(const std::vector<uint8_t>& input) {
  M3Result result = m3_CallV(func_run, input);
  // 提取偏移量为524字节（即2096位，或524 * 8）处的32位整数
  std::vector<uint8_t> bytes = extractUint32AsBytes(mem, 524);
  return bytes;
}

std::vector<uint8_t> makeUnlockResponse(const std::vector<uint8_t>& unlockRequestBuffer, const std::string& deviceName) {
  std::vector<uint8_t> unlockRequest = unlockRequestBuffer;

  uint8_t unknownByte = unlockRequest[5];
  std::vector<uint8_t> nonceBytes(unlockRequest.begin() + 6, unlockRequest.begin() + 8);
  std::vector<uint8_t> mac(unlockRequest.begin() + 8, unlockRequest.begin() + 10);

  uint16_t nonce = (nonceBytes[0] << 8) | nonceBytes[1];
  uint16_t newNonce = nonce + 1;
  std::vector<uint8_t> newNonceBytes = nonce == 0xffff ? std::vector<uint8_t>{ 0x01, 0x00 } : std::vector<uint8_t>{ (newNonce >> 8) & 0xff, newNonce & 0xff };

  // 假设这个函数已经在项目中定义或可以从某个库中获取
  // std::vector<uint8_t> makeUnlockKey(const std::vector<uint8_t>& input);
  // 创建一个新的 vector 来存储合并后的数据  
    std::vector<uint8_t> combinedKey(nonceBytes.begin(), nonceBytes.end());  
    combinedKey.insert(combinedKey.end(), mac.begin(), mac.end());  

  std::vector<uint8_t> rawKey = makeUnlockKey(combinedKey);

  std::vector<uint8_t> mask(4);
  for (int i = 0; i < 4 && i < deviceName.size(); ++i) {
    mask[i] = deviceName[deviceName.size() - 4 + i] - '0';
  }

  std::vector<uint8_t> key(rawKey.size());
  for (size_t i = 0; i < rawKey.size(); ++i) {
    key[i] = rawKey[i] ^ (i < mask.size() ? mask[i] : 0);
  }

  std::vector<uint8_t> checksumInput = {
    unknownByte,
    newNonceBytes[0], newNonceBytes[1],
    key[0], key[1], key[2], key[3],
    0xFE, 0x87, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
  };

  uint8_t checksum = crc16cgaeaf(checksumInput);

  std::vector<uint8_t> finalPayload = {
    0xFE, 0xFE, 0x09, 0xAF,
    static_cast<uint8_t>(checksum),
    checksumInput[0], checksumInput[1], checksumInput[2], checksumInput[3],
    checksumInput[4], checksumInput[5], checksumInput[6], checksumInput[7],
    checksumInput[8], checksumInput[9], checksumInput[10], checksumInput[11],
    checksumInput[12], checksumInput[13], checksumInput[14], checksumInput[15]
  };

  return finalPayload;
}

void disconnect(void){
  if(connected){
    pClient->disconnect();
    connected=false;
      doSacn = true;
  }
}

// 收到服务推送的数据时的回调函数
void NotifyCallback(BLERemoteCharacteristic* characteristic, uint8_t* value, size_t length, bool isNotify) {
    Serial.print("Notify callback for characteristic ");
    Serial.print(characteristic->getUUID().toString().c_str());
    Serial.print(" of data length ");
    Serial.println(length);
    Serial.print("data: ");
    Serial.write(value, length);
    Serial.println();

   //auto value = pData;
   //Serial.printf("该特征值可以读取并且当前值为: %s\r\n", characteristic->readValue().c_str());

  if (length > 0) {
    //char* hexString = hexToString((unsigned char*)value, length);
    Serial.println("Received data on TXD");
    //free(hexString);

    uint8_t dType = value[3];  // "dType" as-is in the original code
    
    if (dType == 0xBA) {  // user info upload request; send BA ack to tell it we have done that (won't actually do it)
                          // send this if BA is received
                          // BA is related to user info uploading, but we will never actually upload anything
      std::vector<uint8_t> response = {
        0xFE, 0xFE, 0x09, 0xBA,
        0x00, 0x00
      };
      txdCharacteristic->writeValue(response.data(), response.size());
    }

    else if (dType == 0xB2) {  // start ok; update ui
      Serial.println("ok");
    }

    else if (dType == 0xBC) {  // see offlinebombFix
      // send this before startEpilogueOfflinebomb and before endEpilogue, or if BC is received
      // used to clear the previous unfinished offline (BB) session
      // receiving BC is a strong signal that sending this is necessary,
      // but if Offlinebomb will be used it will be safer to send this before any other command
      // (deprecated, for reference only)
      std::vector<uint8_t> response = {
        0xFE, 0xFE, 0x09, 0xBC,
        0x00, 0x00
      };
      txdCharacteristic->writeValue(response.data(), response.size());
    }
    else if(dType == 0xB3){
      std::vector<uint8_t> response = {
        0xFE, 0xFE, 0x09, 0xB4,
        0x00, 0x00
      };
      txdCharacteristic->writeValue(response.data(), response.size());
      disconnect();
    }
    if (dType == 0xB0 || dType == 0xB1) {
      // start prologue ok; delay 500ms for key authentication request (AE) if this is a new firmware
      // send this first, then wait for B0, B1, or AE
      // B0 and B1 contain the status of the previous session, AE is a key authentication request
      // AE will only show up in new firmwares
      delay(500);
      std::vector<uint8_t> response = makeStartEpilogue(pServer->getName().c_str());
      txdCharacteristic->writeValue(response.data(), response.size());
    }

    /*

    if (dType == 0xAE) {  // receiving an unlock request (AE), this is a new firmware
      std::vector<uint8_t> vec(value, value + length);  
      std::vector<uint8_t> response = makeUnlockResponse(vec, pServer->getName());
      txdCharacteristic->writeValue(response.data(), response.size());
    }

    // FEFE 09AF: key authentication (new firmware only)
    if (dType == 0xAF) {
            std::vector<uint8_t> response;
            switch (value[5]) {
              case 0x55:  // key authentication ok; continue to send start epilogue (B2)
                response = makeStartEpilogue(pServer->getName().c_str(), true);
                txdCharacteristic->writeValue(response.data(), response.size());
                break;
              case 0x01:  // key authentication failed
              case 0x04:
                Serial.println("WATERCTL INTERNAL Bad key");
              default:
                response = makeStartEpilogue(pServer->getName().c_str(), true);
                txdCharacteristic->writeValue(response.data(), response.size());
                Serial.println("WATERCTL INTERNAL Unknown RXD data");
            }
    }*/
  }
}

// 用来连接设备获取其中的服务与特征
bool ConnectToServer(void) {
  BLEClient* pClient = BLEDevice::createClient();       // 创建客户端
  pClient->setClientCallbacks(new MyClientCallback());  // 添加客户端与服务器连接与断开回调功能
  if (!pClient->connect(pServer)) {                     // 尝试连接设备
    return false;
  }
  Serial.println("连接设备成功");

  BLERemoteService* pRemoteService = pClient->getService(SERVICE_UUID);  // 尝试获取设备中的服务
  if (pRemoteService == nullptr) {
    Serial.println("获取服务失败");
    pClient->disconnect();
    return false;
  }
  Serial.println("获取服务成功");

  /*
    pRemoteCharacteristic = pRemoteService->getCharacteristic(CHARACTERISTIC_UUID); // 尝试获取服务中的特征
    if (pRemoteCharacteristic == nullptr) {
      Serial.println("获取特性失败");
      pClient->disconnect();
      return false;
    }
    Serial.println("获取特征成功");
    txdCharacteristic = await service.getCharacteristic(0xf1f1);
    rxdCharacteristic = await service.getCharacteristic(0xf1f2);
    */
  txdCharacteristic = pRemoteService->getCharacteristic(CHARACTERISTIC_TXD_UUID);  // 尝试获取服务中的特征
  if (txdCharacteristic == nullptr) {
    Serial.println("获取txdCharacteristic特性失败");
    pClient->disconnect();
    return false;
  }
  Serial.println("获取txdCharacteristic特征成功");

  rxdCharacteristic = pRemoteService->getCharacteristic(CHARACTERISTIC_RXD_UUID);  // 尝试获取服务中的特征
  if (rxdCharacteristic == nullptr) {
    Serial.println("获取rxdCharacteristic特性失败");
    pClient->disconnect();
    return false;
  }
  Serial.println("获取rxdCharacteristic特征成功");

  /*
    if(pRemoteCharacteristic->canRead()) { // 如果特征值可以读取则读取数据
      Serial.printf("该特征值可以读取并且当前值为: %s\r\n", pRemoteCharacteristic->readValue().c_str());
    }
    if(pRemoteCharacteristic->canNotify()) { // 如果特征值启用了推送则添加推送接收处理
      pRemoteCharacteristic->registerForNotify(NotifyCallback);
    }
    */
  //   Serial.printf("该特征值可以读取并且当前值为: %s\r\n", rxdCharacteristic->readValue().c_str());
  // if(rxdCharacteristic->canRead()) { // 如果特征值可以读取则读取数据
  //   Serial.printf("该特征值可以读取并且当前值为: %s\r\n", rxdCharacteristic->readValue().c_str());
  // }

  if (rxdCharacteristic->canNotify()) {  // 如果特征值启用了推送则添加推送接收处理
    Serial.printf("添加推送接收处理\r\n");
    rxdCharacteristic->registerForNotify(NotifyCallback);
  }
  if (txdCharacteristic->canWrite()) {  // FEFE 09B0 0101 0000: start prologue
    std::vector<uint8_t> startPrologue = { 0xFE, 0xFE, 0x09, 0xB0, 0x01, 0x01, 0x00, 0x00 };
    Serial.printf("特征写入消息:%s %d\r\n",startPrologue.data(), startPrologue.size());
    txdCharacteristic->writeValue(startPrologue.data(), startPrologue.size());
  }

  
    connected = true;
    return true;
}

#include <WiFi.h>

#define NTP1 "ntp1.aliyun.com"
#define NTP2 "ntp2.aliyun.com"
#define NTP3 "ntp3.aliyun.com"
//填写WIFI入网信息
const char *ssid = "fosu";                                                                                // WIFI账户
const char *password = "";                                                                                 // WIFI密码
const String WDAY_NAMES[] = {"星期天", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};                //星期
const String MONTH_NAMES[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}; //月份

//time_t now; //实例化时间
void setClock()
{

  struct tm timeInfo; //声明一个结构体
  if (!getLocalTime(&timeInfo))
  { //一定要加这个条件判断，否则内存溢出
    Serial.println("Failed to obtain time");
    return;
  }
  //Serial.print(asctime(&timeInfo)); //默认打印格式：Mon Oct 25 11:13:29 2021
  String date = WDAY_NAMES[timeInfo.tm_wday];
  Serial.println(date.c_str());
  // sprintf_P(buff1, PSTR("%04d-%02d-%02d %s"), timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday, WDAY_NAMES[timeInfo.tm_wday].c_str());
  String shuju = String(timeInfo.tm_year + 1900); //年
  shuju += "-";
  shuju += timeInfo.tm_mon + 1; //月
  shuju += "-";
  shuju += timeInfo.tm_mday; //日
  shuju += " ";
  shuju += timeInfo.tm_hour; //时
  shuju += ":";
  shuju += timeInfo.tm_min;
  shuju += ":";
  shuju += timeInfo.tm_sec;
  shuju += " ";
  shuju += WDAY_NAMES[timeInfo.tm_wday].c_str(); //星期
  Serial.println(shuju.c_str());
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  //设置ESP32工作模式为无线终端模式
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connected!");
  configTime(8 * 3600, 0, NTP1, NTP2, NTP3);

  load_wasm();

  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(80);
}

void loop() {

  Serial.println("Waiting 10s before the next round...");
  delay(10000);
  setClock();

  // 如果需要扫描则进行扫描
  if (doSacn) {
    Serial.println("开始搜索设备");
    BLEDevice::getScan()->clearResults();
    BLEDevice::getScan()->start(0);  // 持续搜索设备
  }
  // 如果找到设备就尝试连接设备
  if (doConnect) {
    if (ConnectToServer()) {
      connected = true;
    } else {
      doSacn = true;
    }
    doConnect = false;
  }
  // 如果已经连接就可以向设备发送数据
  // if (connected) {
  //   if(pRemoteCharacteristic->canWrite()) { // 如果可以向特征值写数据
  //     delay(3500);
  //     String newValue = "mytime: " + String(millis()/1000);
  //     Serial.printf("像特征写入消息: %s\r\n", newValue.c_str());
  //     pRemoteCharacteristic->writeValue(newValue.c_str(), newValue.length());
  //   }
  // }
}
