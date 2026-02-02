/*===================================================================*/
/* */
/* InfoNES_System_Linux.cpp : Linux specific File                   */
/* */
/* Refactored for Embedded Linux (Framebuffer + ALSA + Evdev)       */
/* */
/*===================================================================*/

/*-------------------------------------------------------------------*/
/* Include files                                                    */
/*-------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h> // 引入 input 子系统
#include <alsa/asoundlib.h>

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"

/*-------------------------------------------------------------------*/
/* Configuration                                                    */
/*-------------------------------------------------------------------*/

// 请根据实际情况修改输入设备路径
#define INPUT_DEVICE_PATH "/dev/input/event3" 

// NES 按键位定义
#define NES_A       (1 << 0)
#define NES_B       (1 << 1)
#define NES_SELECT  (1 << 2)
#define NES_START   (1 << 3)
#define NES_UP      (1 << 4)
#define NES_DOWN    (1 << 5)
#define NES_LEFT    (1 << 6)
#define NES_RIGHT   (1 << 7)

#define TRUE 1
#define FALSE 0

/*-------------------------------------------------------------------*/
/* Global Variables                                                 */
/*-------------------------------------------------------------------*/

// Sound
static snd_pcm_t *playback_handle = NULL;

// Framebuffer
static int fb_fd = -1;
static unsigned char *fb_mem;
static int px_width;
static int line_width;
static int screen_width;
static int lcd_width;
static int lcd_height;
static struct fb_var_screeninfo var;

static int *zoom_x_tab;
static int *zoom_y_tab;

// Input
static int input_fd = -1;

// ROM Info
char szRomName[256];
char szSaveName[256];
int nSRAM_SaveFlag;

// Thread & State
pthread_t emulation_tid;
int bThread = FALSE;
volatile int g_bLoop = TRUE; // 控制主循环退出

// Pad State (Shared with InfoNES core)
DWORD dwKeyPad1 = 0;
DWORD dwKeyPad2 = 0;
DWORD dwKeySystem = 0;

/*-------------------------------------------------------------------*/
/* Function prototypes                                              */
/*-------------------------------------------------------------------*/

void *emulation_thread(void *args);
void start_application(char *filename);
int LoadSRAM();
int SaveSRAM();
int input_init();
void process_input();

/*-------------------------------------------------------------------*/
/* Display Functions                                                */
/*-------------------------------------------------------------------*/

static inline void rgb555_to_rgb888(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = ((c >> 10) & 0x1F) * 255 / 31;
    *g = ((c >>  5) & 0x1F) * 255 / 31;
    *b = ((c >>  0) & 0x1F) * 255 / 31;
}

static inline uint32_t pack_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t pix = 0;
    uint32_t rv = r >> (8 - var.red.length);
    uint32_t gv = g >> (8 - var.green.length);
    uint32_t bv = b >> (8 - var.blue.length);

    pix |= (rv << var.red.offset);
    pix |= (gv << var.green.offset);
    pix |= (bv << var.blue.offset);

    if (var.transp.length)
        pix |= (((1u << var.transp.length) - 1u) << var.transp.offset);

    return pix;
}

static int lcd_fb_display_px(WORD color, int x, int y)
{
    uint8_t r, g, b;
    rgb555_to_rgb888((uint16_t)color, &r, &g, &b);
    uint32_t pix = pack_pixel(r, g, b);

    // 边界检查，防止越界崩溃
    if (x >= lcd_width || y >= lcd_height) return 0;

    uint8_t *p = (uint8_t *)(fb_mem + y * line_width + x * px_width);

    if (var.bits_per_pixel == 16) {
        *(uint16_t *)p = (uint16_t)pix;
    } else if (var.bits_per_pixel == 32) {
        *(uint32_t *)p = pix;
    }
    return 0;
}

static int lcd_fb_init()
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd == -1) {
        printf("[Display] Can't open /dev/fb0\n");
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) == -1) {
        close(fb_fd);
        printf("[Display] Can't ioctl /dev/fb0\n");
        return -1;
    }

    px_width = var.bits_per_pixel / 8;
    line_width = var.xres * px_width;
    screen_width = var.yres * line_width;
    lcd_width = var.xres;
    lcd_height = var.yres;

    printf("[Display] FB: %dx%d, %dbpp\n", lcd_width, lcd_height, var.bits_per_pixel);

    fb_mem = (unsigned char *)mmap(NULL, screen_width, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == (void *)-1) {
        close(fb_fd);
        printf("[Display] Can't mmap /dev/fb0\n");
        return -1;
    }

    memset(fb_mem, 0, screen_width);
    return 0;
}

int make_zoom_tab()
{
    int i;
    zoom_x_tab = (int *)malloc(sizeof(int) * lcd_width);
    if (!zoom_x_tab) return -1;
    
    for (i = 0; i < lcd_width; i++) {
        zoom_x_tab[i] = i * NES_DISP_WIDTH / lcd_width;
    }

    zoom_y_tab = (int *)malloc(sizeof(int) * lcd_height);
    if (!zoom_y_tab) return -1;

    for (i = 0; i < lcd_height; i++) {
        zoom_y_tab[i] = i * NES_DISP_HEIGHT / lcd_height;
    }
    return 1;
}

/*-------------------------------------------------------------------*/
/* Input Functions (Replaces game_input.c)                          */
/*-------------------------------------------------------------------*/

int input_init()
{
    input_fd = open(INPUT_DEVICE_PATH, O_RDONLY | O_NONBLOCK);
    if (input_fd < 0) {
        printf("[Input] Failed to open %s: %s\n", INPUT_DEVICE_PATH, strerror(errno));
        return -1;
    }
    printf("[Input] Opened %s successfully.\n", INPUT_DEVICE_PATH);
    return 0;
}

void process_input()
{
    struct input_event ev;
    if (input_fd < 0) return;

    while (read(input_fd, &ev, sizeof(ev)) > 0) {
        if (ev.type == EV_KEY) {
            int pressed = ev.value; // 1=Press, 0=Release, 2=Repeat
            if (ev.value == 2) continue; // Ignore repeat for now

            // Helper macro to set/clear bit
            #define KEY_MAP(mask) do { \
                if (pressed) dwKeyPad1 |= (mask); \
                else         dwKeyPad1 &= ~(mask); \
            } while(0)

            switch (ev.code) {
                // 方向键
                case KEY_UP:    KEY_MAP(NES_UP); break;
                case KEY_DOWN:  KEY_MAP(NES_DOWN); break;
                case KEY_LEFT:  KEY_MAP(NES_LEFT); break;
                case KEY_RIGHT: KEY_MAP(NES_RIGHT); break;
                
                // 功能键映射
                case KEY_Z:         KEY_MAP(NES_A); break;      // A
                case KEY_X:         KEY_MAP(NES_B); break;      // B
                case KEY_ENTER:     KEY_MAP(NES_START); break;  // Start
                case KEY_BACKSPACE: KEY_MAP(NES_SELECT); break; // Select
                
                // 退出键
                case KEY_ESC:
                    if (pressed) {
                        printf("[System] ESC pressed. Exiting...\n");
                        g_bLoop = FALSE;
                    }
                    break;
            }
        }
    }
}

/*-------------------------------------------------------------------*/
/* Main Entry Point                                                 */
/*-------------------------------------------------------------------*/

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: %s <rom.nes>\n", argv[0]);
        return 0;
    }

    // 1. Initialize Display
    if (lcd_fb_init() < 0) return -1;
    if (make_zoom_tab() < 0) return -1;

    // 2. Initialize Input
    input_init(); // 不再依赖外部函数

    // 3. Start Game
    start_application(argv[1]);

    // 4. Main Loop (Input Polling)
    while (g_bLoop) {
        process_input();
        usleep(1000); // 1ms sleep to prevent 100% CPU usage in input thread
    }

    // 5. Cleanup
    bThread = FALSE;
    if (emulation_tid) {
        pthread_join(emulation_tid, NULL);
    }

    InfoNES_SoundClose();

    if (fb_mem && fb_mem != (void*)-1) munmap(fb_mem, screen_width);
    if (fb_fd >= 0) close(fb_fd);
    if (input_fd >= 0) close(input_fd);
    if (zoom_x_tab) free(zoom_x_tab);
    if (zoom_y_tab) free(zoom_y_tab);

    return 0;
}

/*-------------------------------------------------------------------*/
/* Emulation Thread                                                 */
/*-------------------------------------------------------------------*/

void *emulation_thread(void *args)
{
    InfoNES_Main();
    g_bLoop = FALSE; // Stop main loop if emulator exits
    return NULL;
}

void start_application(char *filename)
{
    strcpy(szRomName, filename);

    if (InfoNES_Load(szRomName) == 0) {
        LoadSRAM();
        bThread = TRUE;
        pthread_create(&emulation_tid, NULL, emulation_thread, NULL);
    } else {
        printf("[System] Failed to load ROM: %s\n", filename);
        g_bLoop = FALSE;
    }
}

/*-------------------------------------------------------------------*/
/* SRAM Functions                                                   */
/*-------------------------------------------------------------------*/

int LoadSRAM()
{
    FILE *fp;
    unsigned char pSrcBuf[SRAM_SIZE];
    unsigned char chData, chTag;
    int nRunLen, nDecoded, nDecLen, nIdx;

    nSRAM_SaveFlag = 0;
    if (!ROM_SRAM) return 0;
    nSRAM_SaveFlag = 1;

    strcpy(szSaveName, szRomName);
    strcpy(strrchr(szSaveName, '.') + 1, "srm");

    fp = fopen(szSaveName, "rb");
    if (fp == NULL) return -1;

    fread(pSrcBuf, SRAM_SIZE, 1, fp);
    fclose(fp);

    // Simple RLE Decompression
    nDecoded = 0;
    nDecLen = 0;
    chTag = pSrcBuf[nDecoded++];

    while (nDecLen < 8192) {
        chData = pSrcBuf[nDecoded++];
        if (chData == chTag) {
            chData = pSrcBuf[nDecoded++];
            nRunLen = pSrcBuf[nDecoded++];
            for (nIdx = 0; nIdx < nRunLen + 1; ++nIdx)
                SRAM[nDecLen++] = chData;
        } else {
            SRAM[nDecLen++] = chData;
        }
    }
    printf("[System] SRAM Loaded\n");
    return 0;
}

int SaveSRAM()
{
    FILE *fp;
    int nUsedTable[256];
    unsigned char chData, chPrevData, chTag;
    int nIdx, nEncoded, nEncLen, nRunLen;
    unsigned char pDstBuf[SRAM_SIZE];

    if (!nSRAM_SaveFlag) return 0;

    // Simple RLE Compression
    memset(nUsedTable, 0, sizeof nUsedTable);
    for (nIdx = 0; nIdx < SRAM_SIZE; ++nIdx) ++nUsedTable[SRAM[nIdx++]];
    for (nIdx = 1, chTag = 0; nIdx < 256; ++nIdx) {
        if (nUsedTable[nIdx] < nUsedTable[chTag]) chTag = nIdx;
    }

    nEncoded = 0;
    nEncLen = 0;
    nRunLen = 1;
    pDstBuf[nEncLen++] = chTag;
    chPrevData = SRAM[nEncoded++];

    while (nEncoded < SRAM_SIZE && nEncLen < SRAM_SIZE - 133) {
        chData = SRAM[nEncoded++];
        if (chPrevData == chData && nRunLen < 256)
            ++nRunLen;
        else {
            if (nRunLen >= 4 || chPrevData == chTag) {
                pDstBuf[nEncLen++] = chTag;
                pDstBuf[nEncLen++] = chPrevData;
                pDstBuf[nEncLen++] = nRunLen - 1;
            } else {
                for (nIdx = 0; nIdx < nRunLen; ++nIdx)
                    pDstBuf[nEncLen++] = chPrevData;
            }
            chPrevData = chData;
            nRunLen = 1;
        }
    }
    // Flush remaining
    if (nRunLen >= 4 || chPrevData == chTag) {
        pDstBuf[nEncLen++] = chTag;
        pDstBuf[nEncLen++] = chPrevData;
        pDstBuf[nEncLen++] = nRunLen - 1;
    } else {
        for (nIdx = 0; nIdx < nRunLen; ++nIdx)
            pDstBuf[nEncLen++] = chPrevData;
    }

    fp = fopen(szSaveName, "wb");
    if (fp == NULL) return -1;
    fwrite(pDstBuf, nEncLen, 1, fp);
    fclose(fp);
    printf("[System] SRAM Saved\n");
    return 0;
}

/*-------------------------------------------------------------------*/
/* InfoNES Interface Implementation                                 */
/*-------------------------------------------------------------------*/

int InfoNES_Menu()
{
    if (bThread == FALSE) return -1;
    return 0;
}

int InfoNES_ReadRom(const char *pszFileName)
{
    FILE *fp;
    fp = fopen(pszFileName, "rb");
    if (fp == NULL) return -1;

    fread(&NesHeader, sizeof NesHeader, 1, fp);
    if (memcmp(NesHeader.byID, "NES\x1a", 4) != 0) {
        fclose(fp);
        return -1;
    }

    memset(SRAM, 0, SRAM_SIZE);
    if (NesHeader.byInfo1 & 4) {
        fread(&SRAM[0x1000], 512, 1, fp);
    }

    ROM = (BYTE *)malloc(NesHeader.byRomSize * 0x4000);
    fread(ROM, 0x4000, NesHeader.byRomSize, fp);

    if (NesHeader.byVRomSize > 0) {
        VROM = (BYTE *)malloc(NesHeader.byVRomSize * 0x2000);
        fread(VROM, 0x2000, NesHeader.byVRomSize, fp);
    }

    fclose(fp);
    return 0;
}

void InfoNES_ReleaseRom()
{
    if (ROM) { free(ROM); ROM = NULL; }
    if (VROM) { free(VROM); VROM = NULL; }
}

void *InfoNES_MemoryCopy(void *dest, const void *src, int count)
{
    memcpy(dest, src, count);
    return dest;
}

void *InfoNES_MemorySet(void *dest, int c, int count)
{
    memset(dest, c, count);
    return dest;
}

void InfoNES_LoadFrame()
{
    int x, y;
    int line_w;
    WORD wColor;

    if (fb_fd > 0) {
        for (y = 0; y < lcd_height; y++) {
            line_w = zoom_y_tab[y] * NES_DISP_WIDTH;
            for (x = 0; x < lcd_width; x++) {
                wColor = WorkFrame[line_w + zoom_x_tab[x]];
                lcd_fb_display_px(wColor, x, y);
            }
        }
    }
}

void InfoNES_PadState(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
    *pdwPad1 = dwKeyPad1;
    *pdwPad2 = dwKeyPad2;
    *pdwSystem = dwKeySystem;
}

void InfoNES_SoundInit(void) {}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate)
{
    int err;
    unsigned int rate = sample_rate;
    // 尝试多个音频设备
    const char *devices[] = {"default", "plughw:0,0", "plughw:1,0", NULL};
    
    // 清理旧的（如果有）
    if (playback_handle) {
        snd_pcm_close(playback_handle);
        playback_handle = NULL;
    }

    for (int i = 0; devices[i]; i++) {
        if ((err = snd_pcm_open(&playback_handle, devices[i], SND_PCM_STREAM_PLAYBACK, 0)) >= 0) {
            printf("[Audio] Opened device '%s'\n", devices[i]);
            break;
        }
    }

    if (!playback_handle) {
        printf("[Audio] Failed to open any audio device.\n");
        return -1;
    }

    // 设置参数：8位无符号，单声道
    if ((err = snd_pcm_set_params(playback_handle,
                                  SND_PCM_FORMAT_U8,
                                  SND_PCM_ACCESS_RW_INTERLEAVED,
                                  1, // 单声道
                                  rate,
                                  1, // 允许重采样
                                  50000)) < 0) { // 50ms 延迟
        printf("[Audio] Params error: %s\n", snd_strerror(err));
        return -1;
    }

    return 1;
}

void InfoNES_SoundClose(void)
{
    if (playback_handle) {
        snd_pcm_close(playback_handle);
        playback_handle = NULL;
    }
}

void InfoNES_SoundOutput(int samples, BYTE *wave1, BYTE *wave2, BYTE *wave3, BYTE *wave4, BYTE *wave5)
{
    int i;
    int ret;
    unsigned char wav;
    unsigned char *pcmBuf;

    if (!playback_handle) return;

    pcmBuf = (unsigned char *)malloc(samples);
    if (!pcmBuf) return;

    for (i = 0; i < samples; i++) {
        // 简单的混音算法
        wav = (wave1[i] + wave2[i] + wave3[i] + wave4[i] + wave5[i]) / 5;
        pcmBuf[i] = wav;
    }

    ret = snd_pcm_writei(playback_handle, pcmBuf, samples);
    if (ret == -EPIPE) {
        snd_pcm_prepare(playback_handle);
    } else if (ret < 0) {
        // 其他错误处理
    }

    free(pcmBuf);
}

void InfoNES_Wait() {}

void InfoNES_MessageBox(const char *pszMsg, ...)
{
    printf("[InfoNES] Msg: %s\n", pszMsg);
}

/* Palette data (Standard NES Palette) */
WORD NesPalette[64] =
{
    0x39ce, 0x1071, 0x0015, 0x2013, 0x440e, 0x5402, 0x5000, 0x3c20,
    0x20a0, 0x0100, 0x0140, 0x00e2, 0x0ceb, 0x0000, 0x0000, 0x0000,
    0x5ef7, 0x01dd, 0x10fd, 0x401e, 0x5c17, 0x700b, 0x6ca0, 0x6521,
    0x45c0, 0x0240, 0x02a0, 0x0247, 0x0211, 0x0000, 0x0000, 0x0000,
    0x7fff, 0x1eff, 0x2e5f, 0x223f, 0x79ff, 0x7dd6, 0x7dcc, 0x7e67,
    0x7ae7, 0x4342, 0x2769, 0x2ff3, 0x03bb, 0x0000, 0x0000, 0x0000,
    0x7fff, 0x579f, 0x635f, 0x6b3f, 0x7f1f, 0x7f1b, 0x7ef6, 0x7f75,
    0x7f94, 0x73f4, 0x57d7, 0x5bf9, 0x4ffe, 0x0000, 0x0000, 0x0000
};