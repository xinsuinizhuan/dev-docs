/**
 * 示例代码：实现ji.h定义的图像接口，开发者需要根据自己的实际需求对接口进行实现
 */

#include <cstdlib>
#include <cstring>
#include <fstream>

#include <opencv2/opencv.hpp>
#include <glog/logging.h>

#include "encrypt_wrapper.hpp"
#include "ji_license.h"
#include "WKTParser.h"
#include "cJSON.h"
#include <ji_utils.h>
#include "ji.h"

#include "pubKey.hpp"
#include "model_str.hpp"
#include "SampleDetector.hpp"

#define JSON_ALERT_FLAG_KEY ("alert_flag")
#define JSON_ALERT_FLAG_TRUE 1
#define JSON_ALERT_FLAG_FALSE 0

// 如果需要添加授权功能，请保留该宏定义，并在ji_init中实现授权校验
#define ENABLE_JI_AUTHORIZATION
// 如果需要加密模型，请保留该宏定义，并在ji_create_predictor中实现模型解密
#define ENABLE_JI_MODEL_ENCRYPTION

cv::Mat outputFrame;        // 用于存储算法处理后的输出图像，根据ji.h的接口规范，接口实现需要负责释放该资源
char *jsonResult = nullptr; // 用于存储算法处理后输出到JI_EVENT的json字符串，根据ji.h的接口规范，接口实现需要负责释放改资源

// 算法与画图的可配置参数及其默认值
double nms = 0.6;
double thresh = 0.5;
double hierThresh = 0.5;

int gpuID = 0;  // 算法使用的GPU ID，算法必须实现支持从外部设置GPU ID的功能
int textFgColor[3] = {0, 0, 0};         // 检测框顶部文字的颜色
int textBgColor[3] = {255, 255, 255};   // 检测框顶部文字的背景颜色
int dogRectColor[3] = {0, 255, 0};      // 检测框`dog`的颜色
bool drawROIArea = false;           // 是否画ROI
int roiColor[3] = {120, 120, 120};  // ROI框的颜色
bool drawResult = true;         // 是否画检测框
bool drawConfidence = false;    // 是否画置信度

/**
 * 从cJSON数组中获取RGB的三个通道值，并填充到color数组中
 *
 * @param[out] color 填充后的数组
 * @param[in] rgbArr 存储有RGB值的cJSON数组
 */
void getColor(int (&color)[3], cJSON *rgbArr) {
    const int RGB_CHANNEL_SIZE = 3;
    if (int(sizeof(color) / sizeof(int)) != RGB_CHANNEL_SIZE) {
        LOG(ERROR) << "Invalid number of channels!";
        return;
    }
    if (rgbArr == nullptr || rgbArr->type != cJSON_Array || cJSON_GetArraySize(rgbArr) != RGB_CHANNEL_SIZE) {
        LOG(ERROR) << "Invalid rgbArr!";
        return;
    }
    for (int i = 0; i < RGB_CHANNEL_SIZE; ++i) {
        cJSON *channelObj = cJSON_GetArrayItem(rgbArr, i);
        color[i] = channelObj->valueint;
    }
}

/**
 * 解析配置文件`/usr/local/ev_sdk/model/algo_conf.json`
 *
 * @param[in] configFile 配置文件
 * @return 成功解析true，否则返回false
 */
bool parseConfigFile(const char *configFile) {
    LOG(INFO) << "Parsing configuration file: " << configFile;

    std::ifstream confIfs(configFile);
    if (confIfs.is_open()) {
        size_t len = getFileLen(confIfs);
        char *confStr = new char[len + 1];
        confIfs.read(confStr, len);
        confStr[len] = '\0';

        cJSON *confObj = cJSON_Parse(confStr);
        if (confObj == nullptr) {
            LOG(ERROR) << "Failed parsing `" << configFile << "`";
            return false;
        }
        cJSON *gpuObj = cJSON_GetObjectItem(confObj, "gpu_id");
        if (gpuObj != nullptr && gpuObj->type == cJSON_Number) {
            gpuID = gpuObj->valueint;
            LOG(INFO) << "Found gpu_id=" << cJSON_Print(gpuObj);
        }
        cJSON *drawROIObj = cJSON_GetObjectItem(confObj, "draw_roi_area");
        if (drawROIObj != nullptr && (drawROIObj->type == cJSON_True || drawROIObj->type == cJSON_False)) {
            drawROIArea = drawROIObj->valueint;
            LOG(INFO) << "Found draw_roi_area=" << cJSON_Print(drawROIObj);
        }
        if (drawROIArea) {
            cJSON *roiColorObj = cJSON_GetObjectItem(confObj, "roi_color");
            if (roiColorObj != nullptr && roiColorObj->type == cJSON_Array) {
                getColor(roiColor, roiColorObj);
                LOG(INFO) << "Found roi_color=" << cJSON_Print(roiColorObj);
            }
        }
        cJSON *drawResultObj = cJSON_GetObjectItem(confObj, "draw_result");
        if (drawResultObj != nullptr && (drawResultObj->type == cJSON_True || drawResultObj->type == cJSON_False)) {
            drawResult = drawResultObj->valueint;
            LOG(INFO) << "Found draw_result=" << cJSON_Print(drawResultObj);
        }
        cJSON *drawConfObj = cJSON_GetObjectItem(confObj, "draw_confidence");
        if (drawConfObj != nullptr && (drawConfObj->type == cJSON_True || drawConfObj->type == cJSON_False)) {
            drawConfidence = drawConfObj->valueint;
            LOG(INFO) << "Found draw_confidence=" << cJSON_Print(drawConfObj);
        }
        cJSON *nmsObj = cJSON_GetObjectItem(confObj, "nms");
        if (nmsObj != nullptr && nmsObj->type == cJSON_Number) {
            nms = nmsObj->valuedouble;
            LOG(INFO) << "Found nms=" << nms;
        }
        cJSON *threshObj = cJSON_GetObjectItem(confObj, "thresh");
        if (threshObj != nullptr && threshObj->type == cJSON_Number) {
            thresh = threshObj->valuedouble;
            LOG(INFO) << "Found thresh=" << thresh;
        }
        cJSON *hierThreshObj = cJSON_GetObjectItem(confObj, "hier_thresh");
        if (hierThreshObj != nullptr && hierThreshObj->type == cJSON_Number) {
            hierThresh = hierThreshObj->valuedouble;
            LOG(INFO) << "Found hier_thresh=" << hierThresh;
        }
        cJSON *textFgColorObj = cJSON_GetObjectItem(confObj, "text_color");
        if (textFgColorObj != nullptr && textFgColorObj->type == cJSON_Array) {
            LOG(INFO) << "Found text_color=" << cJSON_Print(textFgColorObj);
            getColor(textFgColor, textFgColorObj);
        }
        cJSON *textBgColorObj = cJSON_GetObjectItem(confObj, "text_bg_color");
        if (textBgColorObj != nullptr && textBgColorObj->type == cJSON_Array) {
            LOG(INFO) << "Found text_bg_color=" << cJSON_Print(textBgColorObj);
            getColor(textBgColor, cJSON_GetObjectItem(confObj, "text_bg_color"));
        }
        cJSON *objectColorArr = cJSON_GetObjectItem(confObj, "object_colors");
        if (objectColorArr != nullptr && objectColorArr->type == cJSON_Array) {
            for (int i = 0; i < cJSON_GetArraySize(objectColorArr); ++i) {
                cJSON *obj = cJSON_GetArrayItem(objectColorArr, i);
                if (obj != nullptr && obj->type == cJSON_Object) {
                    cJSON *colorObj = cJSON_GetObjectItem(obj, "dog");
                    if (colorObj != nullptr && colorObj->type == cJSON_Array) {
                        LOG(INFO) << "Found dog rect color=" << cJSON_Print(colorObj);
                        getColor(dogRectColor, colorObj);
                        break;
                    }
                }
            }
        }

        free(confStr);
        cJSON_Delete(confObj);

    } else {
        LOG(ERROR) << "Failed reading `" << configFile << "`";
        return false;
    }
    return true;
}

/**
 * 使用predictor对输入图像inFrame进行处理
 *
 * @param[in] predictor 算法句柄
 * @param[in] inFrame 输入图像
 * @param[in] args 处理当前输入图像所需要的输入参数，例如在目标检测中，通常需要输入ROI，由开发者自行定义和解析
 * @param[out] outFrame 输入图像，由内部填充结果，外部代码需要负责释放其内存空间
 * @param[out] event 以JI_EVENT封装的处理结果
 * @return 如果处理成功，返回JISDK_RET_SUCCEED
 */
int processMat(SampleDetector *detector, const cv::Mat &inFrame, const char* args, cv::Mat &outFrame, JI_EVENT &event) {
    // 处理输入图像
    if (inFrame.empty()) {
        return JISDK_RET_FAILED;
    }
    // 获取ROI，这里的示例代码仅将ROI区域画在输出图中，请根据实际项目需求，对ROI区域进行分析
    std::vector<VectorPoint> polygons;
    if (args != nullptr && strlen(args) > 0) {
        LOG(INFO) << "input args:" << args;
        cJSON *argsObj = cJSON_Parse(args);
        cJSON *roiArrObj = cJSON_GetObjectItem(argsObj, "roi");
        WKTParser wktParser(cv::Size(inFrame.cols, inFrame.rows));

        for (int i = 0; i < cJSON_GetArraySize(roiArrObj); ++i) {
            cJSON *roiObj = cJSON_GetArrayItem(roiArrObj, i);
            if (roiObj == nullptr || roiObj->type != cJSON_String) {
                continue;
            }
            VectorPoint polygonPoints;
            wktParser.parsePolygon(roiObj->valuestring, &polygonPoints);
            polygons.emplace_back(polygonPoints);
            LOG(INFO) << "Found roi=" << cJSON_Print(roiObj);
            LOG(INFO) << "Parsed roi points:";
            for (auto &point: polygonPoints) {
                LOG(INFO) << "Point(" << point.x << ", " << point.y << ")";
            }
        }
    }

    std::vector<SampleDetector::Object> detectResult;
    int processRet = detector->processImage(inFrame, detectResult);
    if (processRet != SampleDetector::PROCESS_OK) {
        return JISDK_RET_FAILED;
    }

    bool isNeedAlert = false;
    std::vector<SampleDetector::Object> dogs;

    // 创建输出图
    inFrame.copyTo(outFrame);
    // 画ROI区域
    if (drawROIArea && !polygons.empty()) {
        drawPolygon(outFrame, polygons, cv::Scalar(roiColor[0], roiColor[1], roiColor[2]), 2);
    }
    // 判断是否要要报警并将检测到的目标画到输出图上
    for (auto &object : detectResult) {
        // 如果检测到有`狗`就报警
        if (strcmp(object.name.c_str(), "dog") == 0) {
            LOG(INFO) << "Found person:" << object.name;
            if (drawResult) {
                std::stringstream ss;
                ss << object.name;
                if (drawConfidence) {
                    ss.precision(2);
                    ss << std::fixed << ": " << object.prob * 100 << "%";
                }
                drawRectAndText(outFrame, object.rect, ss.str(), 4,
                        cv::Scalar(dogRectColor[0], dogRectColor[1], dogRectColor[2]), 30,
                        cv::Scalar(textFgColor[0], textFgColor[1], textFgColor[2]),
                        cv::Scalar(textBgColor[0], textBgColor[1], textBgColor[2]));
            }

            isNeedAlert = true;
            dogs.push_back(object);
        }
    }

    // 将结果封装成json字符串
    cJSON *rootObj = cJSON_CreateObject();
    int jsonAlertCode = JSON_ALERT_FLAG_FALSE;
    if (isNeedAlert) {
        jsonAlertCode = JSON_ALERT_FLAG_TRUE;
    }
    cJSON_AddItemToObject(rootObj, JSON_ALERT_FLAG_KEY, cJSON_CreateNumber(jsonAlertCode));
    cJSON *personsObj = cJSON_CreateArray();
    for (auto &dog : dogs) {
        cJSON *personObj = cJSON_CreateObject();
        int xmin = dog.rect.x;
        int ymin = dog.rect.y;
        int xmax = xmin + dog.rect.width;
        int ymax = ymin + dog.rect.height;
        cJSON_AddItemToObject(personObj, "xmin", cJSON_CreateNumber(xmin));
        cJSON_AddItemToObject(personObj, "ymin", cJSON_CreateNumber(ymin));
        cJSON_AddItemToObject(personObj, "xmax", cJSON_CreateNumber(xmax));
        cJSON_AddItemToObject(personObj, "ymax", cJSON_CreateNumber(ymax));
        cJSON_AddItemToObject(personObj, "confidence", cJSON_CreateNumber(dog.prob));

        cJSON_AddItemToArray(personsObj, personObj);
    }
    cJSON_AddItemToObject(rootObj, "dogs", personsObj);

    char *jsonResultStr = cJSON_Print(rootObj);
    int jsonSize = strlen(jsonResultStr);
    if (jsonResult == nullptr) {
        jsonResult = new char[jsonSize + 1];
    } else if (strlen(jsonResult) < jsonSize) {
        free(jsonResult);   // 如果需要重新分配空间，需要释放资源
        jsonResult = new char[jsonSize + 1];
    }
    strcpy(jsonResult, jsonResultStr);

    // 注意：JI_EVENT.code需要根据需要填充，切勿弄反
    if (isNeedAlert) {
        event.code = JISDK_CODE_ALARM;
    } else {
        event.code = JISDK_CODE_NORMAL;
    }
    event.json = jsonResult;

    return JISDK_RET_SUCCEED;
}

int ji_init(int argc, char **argv) {
    int authCode = JISDK_RET_SUCCEED;
#ifdef ENABLE_JI_AUTHORIZATION
    // 检查license参数
    if (argc < 6) {
        return JISDK_RET_INVALIDPARAMS;
    }

    if (argv[0] == NULL || argv[5] == NULL) {
        return JISDK_RET_INVALIDPARAMS;
    }

    int qps = 0;
    if (argv[4]) qps = atoi(argv[4]);

    // 使用公钥校验授权信息
    int ret = ji_check_license(pubKey, argv[0], argv[1], argv[2], argv[3], qps > 0 ? &qps : NULL, atoi(argv[5]));
    if (ret != EV_SUCCESS) {
        authCode = JISDK_RET_UNAUTHORIZED;
    }
#endif
    if (authCode != JISDK_RET_SUCCEED) {
        return authCode;
    }

    // 从统一的配置文件读取配置参数，实现必须支持从这个统一的配置文件中读取算法&业务逻辑相关的配置参数
    const char *configFile = "/usr/local/ev_sdk/model/algo_config.json";
    parseConfigFile(configFile);

    return JISDK_RET_SUCCEED;
}

void ji_reinit() {
#ifdef ENABLE_JI_AUTHORIZATION
    ji_check_license(NULL, NULL, NULL, NULL, NULL, NULL, 0);
#endif
    if (jsonResult) {
        free(jsonResult);
        jsonResult = nullptr;
    }
}


void *ji_create_predictor(int pdtype) {
#ifdef ENABLE_JI_AUTHORIZATION
    if (ji_check_expire_only() != EV_SUCCESS) {
        return nullptr;
    }
#endif

    auto *detector = new SampleDetector(thresh, nms, hierThresh, gpuID);
    char *decryptedModelStr = nullptr;

#ifdef ENABLE_JI_MODEL_ENCRYPTION
    LOG(INFO) << "Decrypting model...";
    // 使用加密后的模型配置文件
    void *h = CreateEncryptor(model_str.c_str(), model_str.size(), key.c_str());

    // 获取解密后的字符串
    int fileLen = 0;
    decryptedModelStr = (char *) FetchBuffer(h, fileLen);
    char *tmp = new char[fileLen + 1];
    strncpy(tmp, decryptedModelStr, fileLen);
    tmp[fileLen] = '\0';
    decryptedModelStr = tmp;
    LOG(INFO) << "Decrypted model size:" << strlen(decryptedModelStr);

    // 获取解密后的文件句柄
    // file *file = (file *) FetchFile(h);

    DestroyEncrtptor(h);
#else
    // 不使用模型加密功能，直接从模型文件读取
    std::ifstream ifs = std::ifstream("/usr/local/ev_sdk/model/yolov3-tiny.cfg", std::ios::binary);
    long len = getFileLen(ifs);
    decryptedModelStr = new char[len + 1];
    ifs.read(decryptedModelStr, len);
    decryptedModelStr[len] = '\0';
#endif

    int iRet = detector->init("/usr/local/ev_sdk/model/config/coco.names",
            decryptedModelStr,
            "/usr/local/ev_sdk/model/yolov3-tiny.weights");
    if (decryptedModelStr != nullptr) {
        free(decryptedModelStr);
    }
    if (iRet != SampleDetector::INIT_OK) {
        return nullptr;
    }
    LOG(INFO) << "SamplePredictor init OK.";

    return detector;
}

void ji_destroy_predictor(void *predictor) {
    if (predictor == NULL) return;

    auto *detector = reinterpret_cast<SampleDetector *>(predictor);
    detector->unInit();
    delete detector;
}

int ji_calc_frame(void *predictor, const JI_CV_FRAME *inFrame, const char *args,
                  JI_CV_FRAME *outFrame, JI_EVENT *event) {
    if (predictor == NULL || inFrame == NULL) {
        return JISDK_RET_INVALIDPARAMS;
    }

#ifdef ENABLE_JI_AUTHORIZATION
    // 校验license是否过期等
    int licenseRet = ji_check_expire();
    if (licenseRet != EV_SUCCESS) {
        return (licenseRet == EV_OVERMAXQPS) ? JISDK_RET_OVERMAXQPS : JISDK_RET_UNAUTHORIZED;
    }
#endif

    auto *detector = reinterpret_cast<SampleDetector *>(predictor);
    cv::Mat inMat(inFrame->rows, inFrame->cols, inFrame->type, inFrame->data, inFrame->step);
    if (inMat.empty()) {
        return JISDK_RET_FAILED;
    }
    cv::Mat outMat;
    int processRet = processMat(detector, inMat, args, outMat, *event);

    if (processRet == JISDK_RET_SUCCEED) {
        if ((event->code != JISDK_CODE_FAILED) && (!outMat.empty()) && (outFrame)) {
            outFrame->rows = outMat.rows;
            outFrame->cols = outMat.cols;
            outFrame->type = outMat.type();
            outFrame->data = outMat.data;
            outFrame->step = outMat.step;
        }
    }
    return processRet;
}

int ji_calc_buffer(void *predictor, const void *buffer, int length, const char *args, const char *outFile,
                   JI_EVENT *event) {
    if (predictor == NULL || buffer == NULL || length <= 0) {
        return JISDK_RET_INVALIDPARAMS;
    }

#ifdef ENABLE_JI_AUTHORIZATION
    // 校验license是否过期等
    int licenseRet = ji_check_expire();
    if (licenseRet != EV_SUCCESS) {
        return (licenseRet == EV_OVERMAXQPS) ? JISDK_RET_OVERMAXQPS : JISDK_RET_UNAUTHORIZED;
    }
#endif

    auto *classifierPtr = reinterpret_cast<SampleDetector *>(predictor);

    const unsigned char *b = (const unsigned char *) buffer;
    std::vector<unsigned char> vecBuffer(b, b + length);
    cv::Mat inMat = cv::imdecode(vecBuffer, cv::IMREAD_COLOR);
    if (inMat.empty()) {
        return JISDK_RET_FAILED;
    }

    cv::Mat outMat;
    int processRet = processMat(classifierPtr, inMat, args, outMat, *event);

    if (processRet == JISDK_RET_SUCCEED) {
        if ((event->code != JISDK_CODE_FAILED) && (!outMat.empty()) && (outFile)) {
            cv::imwrite(outFile,outMat);
        }
    }
    return processRet;
}

int ji_calc_file(void *predictor, const char *inFile, const char *args, const char *outFile, JI_EVENT *event) {
    if (predictor == NULL || inFile == NULL) {
        return JISDK_RET_INVALIDPARAMS;
    }

#ifdef ENABLE_JI_AUTHORIZATION
    // 校验license是否过期等
    int licenseRet = ji_check_expire();
    if (licenseRet != EV_SUCCESS) {
        return (licenseRet == EV_OVERMAXQPS) ? JISDK_RET_OVERMAXQPS : JISDK_RET_UNAUTHORIZED;
    }
#endif

    auto *classifierPtr = reinterpret_cast<SampleDetector *>(predictor);
    cv::Mat inMat = cv::imread(inFile);
    if (inMat.empty()) {
        return JISDK_RET_FAILED;
    }

    cv::Mat outMat;
    int processRet = processMat(classifierPtr, inMat, args, outMat, *event);
    if (processRet == JISDK_RET_SUCCEED) {
        if ((event->code != JISDK_CODE_FAILED) && (!outMat.empty()) && (outFile)) {
            cv::imwrite(outFile, outMat);
        }
    }

    return processRet;
}

int ji_calc_video_file(void *predictor, const char *infile, const char* args,
                       const char *outfile, const char *jsonfile) {
    // 没有实现的接口必须返回`JISDK_RET_UNUSED`
    return JISDK_RET_UNUSED;
}