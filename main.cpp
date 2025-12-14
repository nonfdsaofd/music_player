// 先定义Windows版本宏，确保通用控件常量生效
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#define _WIN32_IE 0x0700  // 补充IE兼容宏，确保Trackbar常量定义

// 【关键修复】手动定义MinGW缺失的EN_RETURN常量（编辑框回车通知码）
#ifndef EN_RETURN
#define EN_RETURN 0x0001
#endif

// MinGW兼容：禁用高版本UTF-8 API依赖
#define _CRT_NON_CONFORMING_WCSTOK
// 保留UNICODE宏（Windows控件/API需要宽字符）
#define UNICODE
#define _UNICODE

// miniaudio配置：启用宽字符API
#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_WINDOWS_WIDECHAR_PATHS 1  // 启用miniaudio宽字符路径支持
#include "miniaudio.h"
 
#include <windows.h>
#include <commctrl.h>  // Trackbar控件的消息/常量定义
#include <string>
#include <vector>
#include <random>
#include <filesystem>
#include <stdexcept>
#include <locale>
#include <algorithm>    // 用于std::max/std::min
#include <iostream>
#include <thread>       // 新增：std::thread头文件
#include <mutex>        // 新增：互斥锁
#include <condition_variable> // 新增：条件变量

namespace fs = std::filesystem;

// 全局变量定义
#define IDC_LIST_MUSIC    1001
#define IDC_BTN_PREV      1002
#define IDC_BTN_PLAYPAUSE 1003
#define IDC_BTN_NEXT      1004
#define IDC_SLIDER_VOLUME 1005
#define IDC_SLIDER_PROG   1006
#define IDC_RADIO_SINGLE  1007
#define IDC_RADIO_LIST    1008
#define IDC_RADIO_RANDOM  1009
#define ID_TIMER_PROG     1010  // 固定定时器ID（绑定窗口）
#define IDC_EDIT_SEARCH   1011  // 搜索输入框
#define IDC_BTN_SEARCH    1012  // 搜索按钮

// 播放状态枚举
enum PlayState {
    STOPPED,
    PLAYING,
    PAUSED
};

// 循环模式枚举
enum LoopMode {
    LOOP_SINGLE,
    LOOP_LIST,
    LOOP_RANDOM
};

// 核心变更：直接存储UTF-16宽字符路径（Windows原生路径格式）
std::vector<std::wstring> g_musicList;          // 当前显示的音乐列表（可能是过滤后的）
std::vector<std::wstring> g_originalMusicList;  // 原始音乐列表（未过滤）
int g_curIndex = -1;
PlayState g_playState = STOPPED;
LoopMode g_loopMode = LOOP_LIST;
int g_volume = 80;
bool g_isDraggingProgress = false; // 仅通过鼠标消息控制的拖动标记
HWND g_hMainWnd = NULL;            // 保存主窗口句柄
HWND g_hProgressSlider = NULL;     // 保存进度条控件句柄
HWND g_hMusicList = NULL;          // 新增：保存音乐列表框句柄，用于窗口大小自适应
HWND g_hSearchEdit = NULL;         // 新增：保存搜索编辑框句柄，用于子类化
std::wstring g_searchKeyword;      // 当前搜索关键词

// 【新增】默认窗口标题（统一管理）
const std::wstring g_defaultWindowTitle = L"简易音乐播放器（UTF-16路径+MinGW兼容+单声道+搜索功能）";

// miniaudio相关全局变量
ma_engine g_engine;
ma_sound g_sound;
ma_bool32 g_soundInitialized = MA_FALSE;

// 【恢复为原普通mutex】
std::mutex g_engineMutex;                      // 保护g_engine的互斥锁
std::condition_variable g_engineCV;            // 等待引擎初始化的条件变量
bool g_engineInitialized = false;              // 引擎是否初始化完成
bool g_engineInitSuccess = false;              // 引擎初始化是否成功
std::thread g_audioInitThread;                 // 音频引擎初始化线程

// 【优化1】新增缓存变量
float g_totalDuration = 0.0f;       // 缓存当前播放音乐的总时长（秒）
DWORD g_lastProgressUpdate = 0;     // 上次更新进度的时间戳（用于节流）

// 随机数生成器
std::mt19937 g_rng(std::random_device{}());

// -------------------------- 提前声明函数（修复所有未声明错误） --------------------------
std::wstring GetFileName(const std::wstring& wFullPath);
std::wstring CharToWStr(const char* szStr);
std::wstring GetExeDir();
void SearchMusic(const std::wstring& keyword);
void UninitCurrentSound();
void CreateProgressTimer();
void DestroyProgressTimer();
void PlayMusic(int index);
void InitAudioEngine(); // 新增：音频引擎初始化函数
LRESULT CALLBACK SearchEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData); // 新增：编辑框子类过程

// -------------------------- 多字节字符串转宽字符字符串 --------------------------
std::wstring CharToWStr(const char* szStr) {
    if (szStr == nullptr || *szStr == '\0') {
        return L"";
    }
    // 获取转换所需的宽字符长度
    int nLen = MultiByteToWideChar(CP_ACP, 0, szStr, -1, nullptr, 0);
    if (nLen <= 0) {
        return L"";
    }
    // 分配缓冲区并转换
    std::wstring wStr(nLen, 0);
    MultiByteToWideChar(CP_ACP, 0, szStr, -1, &wStr[0], nLen);
    // 移除末尾的终止符
    wStr.pop_back();
    return wStr;
}

// -------------------------- 辅助函数：获取程序所在目录（返回UTF-16宽字符路径） --------------------------
std::wstring GetExeDir() {
    WCHAR szPath[MAX_PATH] = {0};
    // 获取程序路径（原生UTF-16）
    GetModuleFileNameW(NULL, szPath, MAX_PATH);
    fs::path exePath = szPath;
    // 直接返回父目录的宽字符路径
    return exePath.parent_path().wstring();
}

// -------------------------- 加载音乐列表（直接存储UTF-16路径） --------------------------
void LoadMusicList() {
    g_musicList.clear();
    g_originalMusicList.clear(); // 清空原始列表
    
    // 1. 拼接music文件夹路径（UTF-16）
    std::wstring musicDir = GetExeDir() + L"/music/"; // 使用/兼容跨平台，fs会自动处理
    fs::path musicDirPath = musicDir; // 直接用宽字符构造fs::path

    // 若music文件夹不存在则创建
    if (!fs::exists(musicDirPath)) {
        fs::create_directory(musicDirPath);
        // 直接显示宽字符路径提示
        std::wstring tipMsg = L"未找到music文件夹，已自动创建！\n路径：" + musicDir + L"\n请将音乐文件放入该文件夹。";
        MessageBoxW(g_hMainWnd, tipMsg.c_str(), L"提示", MB_ICONINFORMATION);
        return;
    }
    if (!fs::is_directory(musicDirPath)) {
        std::wstring errMsg = L"music路径不是文件夹！\n路径：" + musicDir;
        MessageBoxW(g_hMainWnd, errMsg.c_str(), L"错误", MB_ICONERROR);
        return;
    }

    try {
        // 2. 遍历文件夹，筛选音频文件
        for (const auto& entry : fs::directory_iterator(musicDirPath)) {
            if (entry.is_directory()) continue;

            // 直接获取文件完整UTF-16路径（无编码转换）
            std::wstring fullPath = entry.path().wstring();
            // 获取扩展名（UTF-16）
            std::wstring ext = entry.path().extension().wstring();
            
            // 扩展名转小写（宽字符版本）
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

            // 过滤支持的音频格式
            if (ext == L".mp3" || ext == L".wav" || ext == L".flac" || ext == L".ogg") {
                g_musicList.push_back(fullPath); // 存储原生UTF-16路径
                g_originalMusicList.push_back(fullPath); // 保存到原始列表
            }
        }

        // 填充音乐列表（如果列表框已创建）
        if (g_hMusicList != NULL) {
            // 先清空原有内容
            SendMessageW(g_hMusicList, LB_RESETCONTENT, 0, 0);
            // 重新填充
            for (const auto& path : g_musicList) {
                std::wstring fileName = GetFileName(path);
                SendMessageW(g_hMusicList, LB_ADDSTRING, 0, (LPARAM)fileName.c_str());
            }
        }
    } catch (const fs::filesystem_error& e) {
        // 错误信息转换为宽字符显示（修复：先转宽字符再拼接）
        std::wstring errMsg = L"文件遍历错误：";
        errMsg += CharToWStr(e.what()); // 使用辅助函数转换char*到wstring
        MessageBoxW(NULL, errMsg.c_str(), L"错误", MB_ICONERROR);
    }
}

// 辅助函数：从UTF-16路径中提取文件名（用于列表框显示）
std::wstring GetFileName(const std::wstring& wFullPath) {
    fs::path path(wFullPath);
    return path.filename().wstring();
}

// -------------------------- 搜索音乐功能（核心修改：无结果时不停止播放） --------------------------
void SearchMusic(const std::wstring& keyword) {
    g_searchKeyword = keyword;
    g_musicList.clear();

    // 如果关键词为空，恢复原始列表
    if (keyword.empty()) {
        g_musicList = g_originalMusicList;
    } else {
        // 转换关键词为小写，实现不区分大小写搜索
        std::wstring lowerKeyword = keyword;
        std::transform(lowerKeyword.begin(), lowerKeyword.end(), lowerKeyword.begin(), ::towlower);

        // 遍历原始列表，筛选包含关键词的文件
        for (const auto& path : g_originalMusicList) {
            std::wstring fileName = GetFileName(path);
            std::wstring lowerFileName = fileName;
            std::transform(lowerFileName.begin(), lowerFileName.end(), lowerFileName.begin(), ::towlower);

            // 检查文件名是否包含关键词
            if (lowerFileName.find(lowerKeyword) != std::wstring::npos) {
                g_musicList.push_back(path);
            }
        }
    }

    // 更新列表框显示
    if (g_hMusicList != NULL) {
        SendMessageW(g_hMusicList, LB_RESETCONTENT, 0, 0);
        for (const auto& path : g_musicList) {
            std::wstring fileName = GetFileName(path);
            SendMessageW(g_hMusicList, LB_ADDSTRING, 0, (LPARAM)fileName.c_str());
        }
    }

    // 【核心修改】调整无搜索结果的处理逻辑：仅重置索引，不停止播放
    if (g_curIndex >= (int)g_musicList.size()) {
        if (!g_musicList.empty()) {
            g_curIndex = 0; // 重置到第一个搜索结果
        } else {
            // 无搜索结果时：仅重置索引，保留当前播放状态/音频资源/进度定时器
            g_curIndex = -1; 
            // 【移除所有停止播放的逻辑】
            // 不调用UninitCurrentSound、不修改playState、不销毁定时器、不清空进度条、不修改按钮文字
        }
    }
}

// 【优化2】修改UninitCurrentSound：清空缓存 + 恢复默认标题
void UninitCurrentSound() {
    std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
    if (g_soundInitialized) {
        ma_sound_uninit(&g_sound);
        g_soundInitialized = MA_FALSE;
    }
    // 清空总时长缓存和进度更新时间戳
    g_totalDuration = 0.0f;
    g_lastProgressUpdate = 0;

    // 停止播放时恢复默认窗口标题
    if (g_playState == STOPPED && g_hMainWnd != NULL) {
        SetWindowTextW(g_hMainWnd, g_defaultWindowTitle.c_str());
    }
}

// 安全创建进度定时器
void CreateProgressTimer() {
    if (g_hMainWnd == NULL) return;
    KillTimer(g_hMainWnd, ID_TIMER_PROG); // 先销毁旧定时器
    // 恢复原1000ms间隔
    UINT_PTR timerID = SetTimer(g_hMainWnd, ID_TIMER_PROG, 1000, NULL);
    if (timerID == 0) {
        MessageBoxW(g_hMainWnd, L"定时器创建失败！", L"警告", MB_ICONWARNING);
    }
}

// 安全销毁进度定时器
void DestroyProgressTimer() {
    if (g_hMainWnd != NULL) {
        KillTimer(g_hMainWnd, ID_TIMER_PROG);
    }
}

// 【新增】音频引擎初始化函数（放到子线程执行）
void InitAudioEngine() {
    std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels   = 1;        // Must be set when not using a device.
    engineConfig.sampleRate = 48000;    // 采样率（通用48000Hz）

    // 初始化音频引擎（使用自定义配置）
    ma_result result = ma_engine_init(&engineConfig, &g_engine);
    if (result != MA_SUCCESS) {
        g_engineInitSuccess = false;
    } else {
        g_engineInitSuccess = true;
    }

    g_engineInitialized = true; // 标记初始化完成
    g_engineCV.notify_one();   // 通知主线程
}

// 【优化4】修改PlayMusic：缓存总时长 + 加锁保护g_engine + 更新窗口标题
void PlayMusic(int index) {
    if (index < 0 || index >= g_musicList.size()) return;
    if (!g_engineInitSuccess) { // 引擎未初始化成功则返回
        MessageBoxW(g_hMainWnd, L"音频引擎未初始化成功，无法播放音乐！", L"错误", MB_ICONERROR);
        return;
    }

    UninitCurrentSound(); // 释放当前播放的声音

    // 直接获取UTF-16路径（无编码转换）
    std::wstring filePath = g_musicList[index];
    if (filePath.empty()) {
        MessageBoxW(g_hMainWnd, L"文件路径为空！", L"加载失败", MB_ICONERROR);
        return;
    }

    // 关键修改：加锁访问g_engine
    std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
    // 调用miniaudio宽字符版本API，直接传UTF-16路径
    ma_result result = ma_sound_init_from_file_w(&g_engine, filePath.c_str(), 0, NULL, NULL, &g_sound);
    if (result != MA_SUCCESS) {
        // 增强错误提示：显示具体的文件名
        std::wstring errMsg = L"无法加载音乐文件：\n" + GetFileName(g_musicList[index]);
        errMsg += L"\n错误码：" + std::to_wstring(result); // 追加错误码便于调试
        MessageBoxW(g_hMainWnd, errMsg.c_str(), L"错误", MB_ICONERROR);
        return;
    }

    g_soundInitialized = MA_TRUE;
    
    // 【核心优化】仅在播放开始时获取一次总时长并缓存
    ma_result durRes = ma_sound_get_length_in_seconds(&g_sound, &g_totalDuration);
    if (durRes != MA_SUCCESS || g_totalDuration <= 0.0f) {
        g_totalDuration = 0.0f; // 标记无效时长
    }

    ma_sound_set_volume(&g_sound, g_volume / 100.0f); // 设置音量
    ma_sound_set_looping(&g_sound, g_loopMode == LOOP_SINGLE ? MA_TRUE : MA_FALSE); // 设置单曲循环
    ma_sound_start(&g_sound); // 开始播放

    // 更新全局状态
    g_curIndex = index;
    g_playState = PLAYING;
    g_lastProgressUpdate = GetTickCount(); // 重置进度更新时间戳
    CreateProgressTimer(); // 启动进度更新定时器

    // 更新列表选中状态
    if (g_hMusicList != NULL) {
        SendMessageW(g_hMusicList, LB_SETCURSEL, index, 0);
    }
    
    // 更新播放按钮文字
    SetWindowTextW(GetDlgItem(g_hMainWnd, IDC_BTN_PLAYPAUSE), L"暂停");

    // 【核心新增】设置窗口标题为正在播放的音乐名
    std::wstring playingFileName = GetFileName(g_musicList[index]);
    std::wstring newTitle = L"正在播放：" + playingFileName + L" - " + g_defaultWindowTitle;
    SetWindowTextW(g_hMainWnd, newTitle.c_str());
}

// 【优化5】修改TogglePlayPause：暂停时重置时间戳 + 加锁保护
void TogglePlayPause() {
    if (!g_engineInitSuccess) { // 引擎未初始化成功则返回
        MessageBoxW(g_hMainWnd, L"音频引擎未初始化成功，无法播放音乐！", L"错误", MB_ICONERROR);
        return;
    }

    if (!g_soundInitialized || g_musicList.empty()) {
        // 无音乐加载时，默认播放第一首
        if (!g_musicList.empty()) {
            PlayMusic(0);
            SetWindowTextW(GetDlgItem(g_hMainWnd, IDC_BTN_PLAYPAUSE), L"暂停");
        } else {
            MessageBoxW(g_hMainWnd, L"当前无可用音乐列表！", L"提示", MB_ICONINFORMATION);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
    if (g_playState == PLAYING) {
        ma_sound_stop(&g_sound);
        g_playState = PAUSED;
        SetWindowTextW(GetDlgItem(g_hMainWnd, IDC_BTN_PLAYPAUSE), L"播放");
        g_lastProgressUpdate = 0; // 暂停时重置，恢复播放后重新计时
        // 暂停时保持正在播放的标题（如需改为"暂停播放：xxx"可在此修改）
    } else if (g_playState == PAUSED) {
        ma_sound_start(&g_sound);
        g_playState = PLAYING;
        SetWindowTextW(GetDlgItem(g_hMainWnd, IDC_BTN_PLAYPAUSE), L"暂停");
        g_lastProgressUpdate = GetTickCount(); // 恢复播放时重置时间戳
        // 恢复播放时保持正在播放的标题
    } else if (g_playState == STOPPED) {
        PlayMusic(0);
        SetWindowTextW(GetDlgItem(g_hMainWnd, IDC_BTN_PLAYPAUSE), L"暂停");
    }
}

// 下一首（兼容无搜索结果的情况）
void NextMusic() {
    // 若当前列表为空，直接返回（保留当前播放状态）
    if (g_musicList.empty() || !g_engineInitSuccess) return;

    int nextIndex = g_curIndex;
    switch (g_loopMode) {
        case LOOP_SINGLE: nextIndex = g_curIndex; break;
        case LOOP_LIST: nextIndex = (g_curIndex + 1) % g_musicList.size(); break;
        case LOOP_RANDOM:
            if (g_musicList.size() > 1) {
                do { nextIndex = g_rng() % g_musicList.size(); } while (nextIndex == g_curIndex);
            } else { nextIndex = 0; }
            break;
    }
    PlayMusic(nextIndex); // 自动更新标题
}

// 上一首（兼容无搜索结果的情况）
void PrevMusic() {
    // 若当前列表为空，直接返回（保留当前播放状态）
    if (g_musicList.empty() || !g_engineInitSuccess) return;

    int prevIndex = g_curIndex;
    switch (g_loopMode) {
        case LOOP_SINGLE: prevIndex = g_curIndex; break;
        case LOOP_LIST: prevIndex = (g_curIndex - 1 + g_musicList.size()) % g_musicList.size(); break;
        case LOOP_RANDOM:
            if (g_musicList.size() > 1) {
                do { prevIndex = g_rng() % g_musicList.size(); } while (prevIndex == g_curIndex);
            } else { prevIndex = 0; }
            break;
    }
    PlayMusic(prevIndex); // 自动更新标题
}

// 【核心修改：仅缩小UpdateProgressBar的锁粒度】
void UpdateProgressBar() {
    if (g_isDraggingProgress) return;
    // 基础校验：播放状态、资源初始化、总时长有效
    if (g_hMainWnd == NULL || g_playState != PLAYING || !g_soundInitialized || 
        g_hProgressSlider == NULL || g_totalDuration <= 0.0f || !g_engineInitSuccess) {
        return;
    }

    // 节流控制：至少间隔100ms才更新（避免定时器精度问题导致高频调用）
    DWORD currentTick = GetTickCount();
    if (currentTick - g_lastProgressUpdate < 1000) {
        return;
    }
    g_lastProgressUpdate = currentTick;

    float curSeconds = 0.0f;
    bool isAtEnd = false;

    // 【唯一核心修改】缩小锁粒度：仅在获取进度/判断结束时加锁，获取后立即解锁
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        // 获取当前进度
        ma_result res = ma_sound_get_cursor_in_seconds(&g_sound, &curSeconds);
        if (res != MA_SUCCESS) return;

        // 提前判断是否播放结束（避免后续再次加锁）
        isAtEnd = ma_sound_at_end(&g_sound);
    } // 锁在此处自动释放

    // 边界保护：进度不超过总时长
    curSeconds = std::min(curSeconds, g_totalDuration);

    // 更新进度条位置（0-1000范围）
    int pos = static_cast<int>((curSeconds / g_totalDuration) * 1000);
    SendMessageW(g_hProgressSlider, TBM_SETPOS, TRUE, pos);

    // 播放结束判断：解锁后再调用NextMusic，避免嵌套加锁
    const float END_THRESHOLD = 0.1f; // 预留0.1秒误差，避免精度问题
    if ((curSeconds >= g_totalDuration - END_THRESHOLD) && isAtEnd) {
        NextMusic();
    }
}

// 【关键修复】进度条调整后的统一处理（移除g_curIndex < 0的校验）
void OnProgressAdjust() {
    // 移除 g_curIndex < 0 的校验条件，仅保留核心播放状态校验
    if (!g_soundInitialized || g_playState != PLAYING || g_hProgressSlider == NULL || !g_engineInitSuccess) return;
    // 总时长无效时跳过
    if (g_totalDuration <= 0.0f) return;

    int pos = SendMessageW(g_hProgressSlider, TBM_GETPOS, 0, 0);
    float seekSeconds = (pos / 1000.0f) * g_totalDuration;
    seekSeconds = std::max(0.0f, std::min(seekSeconds, g_totalDuration)); // 范围校验
    
    // 加锁调整进度
    std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
    ma_sound_seek_to_second(&g_sound, seekSeconds);
    // 强制更新进度和时间戳
    g_lastProgressUpdate = GetTickCount();
    UpdateProgressBar();
}

// 进度条子类窗口过程（捕获鼠标拖动事件）
LRESULT CALLBACK ProgressSliderSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    switch (uMsg) {
        case WM_LBUTTONDOWN: {
            g_isDraggingProgress = true;
            break;
        }
        case WM_LBUTTONUP: {
            if (g_isDraggingProgress) {
                g_isDraggingProgress = false;
                OnProgressAdjust(); // 拖动结束更新播放进度
            }
            break;
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// 【核心】搜索编辑框子类过程（仅保留回车按键捕获）
LRESULT CALLBACK SearchEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    switch (uMsg) {
        // 仅捕获键盘回车按键，直接执行搜索逻辑
        case WM_KEYDOWN: {
            if (wParam == VK_RETURN) {
                // 直接获取编辑框内容并执行搜索
                WCHAR szKeyword[256] = {0};
                GetWindowTextW(hWnd, szKeyword, 256);
                SearchMusic(szKeyword);
                // 返回0表示已处理，避免传递给默认过程
                return 0;
            }
            break;
        }
        // 子类销毁时清理
        case WM_NCDESTROY: {
            RemoveWindowSubclass(hWnd, SearchEditSubclassProc, 0);
            break;
        }
    }
    // 其他消息交给默认子类过程处理
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// 主窗口过程函数
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            g_hMainWnd = hWnd; // 保存主窗口句柄

            // 【核心修改】启动音频引擎初始化线程
            g_audioInitThread = std::thread(InitAudioEngine);

            // 等待音频引擎初始化完成（主线程阻塞直到初始化完成，避免UI操作未初始化的引擎）
            std::unique_lock<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
            g_engineCV.wait(lock, []() { return g_engineInitialized; });

            // 检查引擎初始化结果
            if (!g_engineInitSuccess) {
                MessageBoxW(0, L"音频引擎初始化失败！", L"错误", MB_ICONERROR);
                return -1;
            }

            RECT rc;
            GetClientRect(hWnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;

            // 1. 创建音乐列表（核心修改：移除LBS_DISABLENOSCROLL，用数值0x0400替代LBS_HASVERTICALSCROLL）
            g_hMusicList = CreateWindowW(L"LISTBOX", L"", 
                WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | 0x0400 | WS_VSCROLL, // 0x0400 = LBS_HASVERTICALSCROLL
                0, 0, 300, height - 50, hWnd, (HMENU)IDC_LIST_MUSIC, 
                ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            // 加载音乐列表（此时列表框已创建，可直接填充）
            LoadMusicList();

            // 2. 创建搜索控件
            int searchY = 10;
            CreateWindowW(L"STATIC", L"搜索：", WS_CHILD | WS_VISIBLE,
                310, searchY, 50, 20, hWnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            
            // 编辑框添加WS_TABSTOP样式（可获取焦点），并保存句柄
            g_hSearchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, // 新增WS_TABSTOP
                370, searchY, 150, 20, hWnd, (HMENU)IDC_EDIT_SEARCH, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            
            // 子类化编辑框（核心：仅捕获回车按键）
            if (!SetWindowSubclass(g_hSearchEdit, SearchEditSubclassProc, 0, 0)) {
                MessageBoxW(hWnd, L"搜索框子类化失败！", L"警告", MB_ICONWARNING);
            }

            CreateWindowW(L"BUTTON", L"搜索", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                530, searchY, 60, 20, hWnd, (HMENU)IDC_BTN_SEARCH, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            // 3. 创建控制按钮（调整Y坐标，避免和搜索控件重叠）
            int btnY = searchY + 30;
            CreateWindowW(L"BUTTON", L"上一首", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                310, btnY, 80, 30, hWnd, (HMENU)IDC_BTN_PREV, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            CreateWindowW(L"BUTTON", L"播放", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                400, btnY, 80, 30, hWnd, (HMENU)IDC_BTN_PLAYPAUSE, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            CreateWindowW(L"BUTTON", L"下一首", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                490, btnY, 80, 30, hWnd, (HMENU)IDC_BTN_NEXT, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            // 4. 创建音量滑块
            btnY += 50;
            CreateWindowW(L"STATIC", L"音量：", WS_CHILD | WS_VISIBLE,
                310, btnY, 50, 20, hWnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            HWND hVolSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                370, btnY, 200, 30, hWnd, (HMENU)IDC_SLIDER_VOLUME, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessageW(hVolSlider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
            SendMessageW(hVolSlider, TBM_SETPOS, TRUE, g_volume);

            // 5. 创建进度滑块（保存句柄）
            btnY += 50;
            CreateWindowW(L"STATIC", L"进度：", WS_CHILD | WS_VISIBLE,
                310, btnY, 50, 20, hWnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            g_hProgressSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                370, btnY, 200, 30, hWnd, (HMENU)IDC_SLIDER_PROG, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessageW(g_hProgressSlider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 1000));
            SendMessageW(g_hProgressSlider, TBM_SETPOS, TRUE, 0);
            
            // 子类化进度条，捕获鼠标拖动事件
            if (!SetWindowSubclass(g_hProgressSlider, ProgressSliderSubclassProc, 0, 0)) {
                MessageBoxW(hWnd, L"进度条子类化失败！", L"警告", MB_ICONWARNING);
            }

            // 6. 创建循环模式单选框
            btnY += 50;
            CreateWindowW(L"BUTTON", L"单曲循环", WS_CHILD | WS_VISIBLE | BS_RADIOBUTTON,
                310, btnY, 80, 20, hWnd, (HMENU)IDC_RADIO_SINGLE, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            HWND hRadioList = CreateWindowW(L"BUTTON", L"列表循环", WS_CHILD | WS_VISIBLE | BS_RADIOBUTTON,
                400, btnY, 80, 20, hWnd, (HMENU)IDC_RADIO_LIST, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessageW(hRadioList, BM_SETCHECK, BST_CHECKED, 0); // 默认选中列表循环
            CreateWindowW(L"BUTTON", L"随机播放", WS_CHILD | WS_VISIBLE | BS_RADIOBUTTON,
                490, btnY, 80, 20, hWnd, (HMENU)IDC_RADIO_RANDOM, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            break;
        }

        // 废弃：主窗口WM_KEYDOWN无法捕获编辑框按键，已删除
        case WM_KEYDOWN: {
            break;
        }

        // 新增：处理窗口大小变化，让列表框自适应
        case WM_SIZE: {
            if (g_hMusicList != NULL) {
                RECT rc;
                GetClientRect(hWnd, &rc);
                // 列表框宽度固定300，高度占满客户端区域（底部留50给其他控件）
                SetWindowPos(g_hMusicList, NULL, 0, 0, 300, rc.bottom - 50, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            break;
        }

        // 可选优化：支持鼠标滚轮滚动列表框（修复POINTS结构体访问错误）
        case WM_MOUSEWHEEL: {
            if (g_hMusicList != NULL) {
                // 提取鼠标坐标并转换为客户端坐标
                POINTS pts = MAKEPOINTS(lParam);
                POINT pt = {pts.x, pts.y}; // 将POINTS转为POINT
                ScreenToClient(g_hMusicList, &pt); // 转换为列表框客户端坐标
                
                // 判断鼠标是否在列表框内
                RECT rcList;
                GetClientRect(g_hMusicList, &rcList);
                if (PtInRect(&rcList, pt)) {
                    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    int curSel = SendMessageW(g_hMusicList, LB_GETCURSEL, 0, 0);
                    if (curSel == LB_ERR) curSel = 0;
                    int newSel = curSel - (delta / WHEEL_DELTA);
                    newSel = std::max(0, std::min(newSel, (int)g_musicList.size() - 1));
                    SendMessageW(g_hMusicList, LB_SETCURSEL, newSel, 0);
                    // 滚动列表到选中项
                    SendMessageW(g_hMusicList, LB_SETTOPINDEX, newSel - 2, 0);
                    return 0;
                }
            }
            break;
        }

        case WM_COMMAND: {
            int ctrlID = LOWORD(wParam);
            int notifyCode = HIWORD(wParam);

            // 双击列表项播放音乐（列表为空时无响应）
            if (ctrlID == IDC_LIST_MUSIC && notifyCode == LBN_DBLCLK) {
                if (g_hMusicList != NULL && !g_musicList.empty()) {
                    int selIndex = SendMessageW(g_hMusicList, LB_GETCURSEL, 0, 0);
                    if (selIndex != LB_ERR) {
                        PlayMusic(selIndex); // 自动更新标题
                    }
                }
            }

            // 按钮/单选框点击处理
            switch (ctrlID) {
                case IDC_BTN_SEARCH: {
                    // 搜索按钮点击：直接获取编辑框内容执行搜索
                    HWND hEdit = GetDlgItem(hWnd, IDC_EDIT_SEARCH);
                    if (hEdit != NULL) {
                        WCHAR szKeyword[256] = {0};
                        GetWindowTextW(hEdit, szKeyword, 256);
                        SearchMusic(szKeyword);
                    }
                    break;
                }
                case IDC_BTN_PREV:
                    PrevMusic();
                    break;
                case IDC_BTN_PLAYPAUSE:
                    TogglePlayPause();
                    break;
                case IDC_BTN_NEXT:
                    NextMusic();
                    break;

                // 单选框UI同步
                case IDC_RADIO_SINGLE: {
                    g_loopMode = LOOP_SINGLE;
                    HWND hSingle = GetDlgItem(hWnd, IDC_RADIO_SINGLE);
                    HWND hList = GetDlgItem(hWnd, IDC_RADIO_LIST);
                    HWND hRandom = GetDlgItem(hWnd, IDC_RADIO_RANDOM);
                    SendMessageW(hSingle, BM_SETCHECK, BST_CHECKED, 0);
                    SendMessageW(hList, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(hRandom, BM_SETCHECK, BST_UNCHECKED, 0);
                    if (g_soundInitialized && g_engineInitSuccess) {
                        std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
                        ma_sound_set_looping(&g_sound, MA_TRUE);
                    }
                    break;
                }
                case IDC_RADIO_LIST: {
                    g_loopMode = LOOP_LIST;
                    HWND hSingle = GetDlgItem(hWnd, IDC_RADIO_SINGLE);
                    HWND hList = GetDlgItem(hWnd, IDC_RADIO_LIST);
                    HWND hRandom = GetDlgItem(hWnd, IDC_RADIO_RANDOM);
                    SendMessageW(hSingle, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(hList, BM_SETCHECK, BST_CHECKED, 0);
                    SendMessageW(hRandom, BM_SETCHECK, BST_UNCHECKED, 0);
                    if (g_soundInitialized && g_engineInitSuccess) {
                        std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
                        ma_sound_set_looping(&g_sound, MA_FALSE);
                    }
                    break;
                }
                case IDC_RADIO_RANDOM: {
                    g_loopMode = LOOP_RANDOM;
                    HWND hSingle = GetDlgItem(hWnd, IDC_RADIO_SINGLE);
                    HWND hList = GetDlgItem(hWnd, IDC_RADIO_LIST);
                    HWND hRandom = GetDlgItem(hWnd, IDC_RADIO_RANDOM);
                    SendMessageW(hSingle, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(hList, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(hRandom, BM_SETCHECK, BST_CHECKED, 0);
                    if (g_soundInitialized && g_engineInitSuccess) {
                        std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
                        ma_sound_set_looping(&g_sound, MA_FALSE);
                    }
                    break;
                }
            }
            break;
        }

        case WM_HSCROLL: {
            HWND hSlider = (HWND)lParam;
            UINT notifyCode = HIWORD(wParam);

            // 音量滑块调整
            if (hSlider == GetDlgItem(hWnd, IDC_SLIDER_VOLUME)) {
                g_volume = SendMessageW(hSlider, TBM_GETPOS, 0, 0);
                if (g_soundInitialized && g_engineInitSuccess) {
                    std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
                    ma_sound_set_volume(&g_sound, g_volume / 100.0f);
                }
            }
            // 进度滑块点击/小步调整（非拖动状态）
            else if (hSlider == g_hProgressSlider && !g_isDraggingProgress) {
                if (notifyCode == TB_PAGEUP || notifyCode == TB_PAGEDOWN || 
                    notifyCode == TB_LINEUP || notifyCode == TB_LINEDOWN) {
                    OnProgressAdjust();
                }
            }
            break;
        }

        // 定时器消息：更新进度条
        case WM_TIMER: {
            if (wParam == ID_TIMER_PROG) {
                UpdateProgressBar();
            }
            break;
        }

        // 窗口销毁：释放所有资源
        case WM_DESTROY: {
            DestroyProgressTimer();
            UninitCurrentSound();
            
            // 等待音频初始化线程结束，再释放引擎
            if (g_audioInitThread.joinable()) {
                g_audioInitThread.join();
            }
            
            // 加锁释放音频引擎
            std::lock_guard<std::mutex> lock(g_engineMutex); // 恢复原普通mutex
            if (g_engineInitSuccess) {
                ma_engine_uninit(&g_engine);
            }

            g_hMainWnd = NULL;
            g_hProgressSlider = NULL;
            g_hMusicList = NULL; // 清空列表框句柄
            g_hSearchEdit = NULL; // 清空编辑框句柄
            PostQuitMessage(0);
            break;
        }

        default:
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    return 0;
}

// 程序入口（MinGW兼容）
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 注册窗口类（宽字符）
    const wchar_t CLASS_NAME[] = L"MusicPlayerClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    // 核心修改：使用系统按钮背景色作为窗口背景（适配系统主题）
    wc.hbrBackground = (HBRUSH)GetSysColorBrush(COLOR_BTNFACE);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"窗口类注册失败！", L"错误", MB_ICONERROR);
        return 0;
    }

    // 创建主窗口（使用统一的默认标题）
    HWND hWnd = CreateWindowW(CLASS_NAME, g_defaultWindowTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 650, 480, // 调整窗口高度，适配搜索控件
        NULL, NULL, hInstance, NULL);

    if (hWnd == NULL) {
        MessageBoxW(NULL, L"窗口创建失败！", L"错误", MB_ICONERROR);
        return 0;
    }

    // 显示并更新窗口
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // 消息循环（宽字符）
    MSG msg = {0};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}