
#include <stdint.h>

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"
#include "../NanoArch/src/core_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

//bool define
#define TRUE 1
#define FALSE 0

/* lcd */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>

#include <fcntl.h>

static snd_pcm_t *playback_handle = NULL;
static int fb_fd = -1;
static unsigned char *fb_mem = NULL;
static int px_width;
static int line_width;
static int screen_width;
static int lcd_width;
static int lcd_height;
static struct fb_var_screeninfo var;
static int *zoom_x_tab;
static int *zoom_y_tab;

extern int InitGameInput(void);
extern int GetGameInput(void);
extern int ExitGameInput(void);

extern int AdapterInitGameInput(void);
extern int AdapterGetGameInput(void);
extern int AdapterExitGameInput(void);

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
    // rgb565_to_rgb888((uint16_t)color, &r, &g, &b);
	rgb555_to_rgb888((uint16_t)color, &r, &g, &b);
    uint32_t pix = pack_pixel(r, g, b);

    uint8_t *p = (uint8_t *)(fb_mem + y * line_width + x * px_width);

    if (var.bits_per_pixel == 16) {
        *(uint16_t *)p = (uint16_t)pix;
    } else if (var.bits_per_pixel == 32) {
        *(uint32_t *)p = pix;
    } else {
        // no
    }
    return 0;
}

static int lcd_fb_init()
{
	fb_fd = open("/dev/fb0", O_RDWR);
	if(-1 == fb_fd)
	{
		printf("cat't open /dev/fb0 \n");
		return -1;
	}

	if(-1 == ioctl(fb_fd, FBIOGET_VSCREENINFO, &var))
	{
		close(fb_fd);
		printf("cat't ioctl /dev/fb0 \n");
		return -1;
	}

	px_width     = var.bits_per_pixel / 8;
	line_width   = var.xres * px_width;
	screen_width = var.yres * line_width;
	lcd_width    = var.xres;
	lcd_height   = var.yres;
	
	printf("fb width:%d height:%d \n", lcd_width, lcd_height);

	fb_mem = (unsigned char *)mmap(NULL, screen_width, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if(fb_mem == (void *)-1)
	{
		close(fb_fd);
		printf("cat't mmap /dev/fb0 \n");
		return -1;
	}

	memset(fb_mem, 0 , screen_width);
	return 0;
}

int make_zoom_tab()
{
	int i;
	zoom_x_tab = (int *)malloc(sizeof(int) * lcd_width);

	if(NULL == zoom_x_tab)
	{
		printf("make zoom_x_tab error\n");
		return -1;
	}
	for(i=0; i<lcd_width; i++)
	{
		zoom_x_tab[i] = i*NES_DISP_WIDTH/lcd_width;
	}
	zoom_y_tab = (int *)malloc(sizeof(int) * lcd_height);
	if(NULL == zoom_y_tab)
	{
		printf("make zoom_y_tab error\n");
		return -1;
	}
	for(i=0; i<lcd_height; i++)
	{
		zoom_y_tab[i] = i*NES_DISP_HEIGHT/lcd_height;
	}
	return 1;
}

/*-------------------------------------------------------------------*/
/*  ROM image file information                                       */
/*-------------------------------------------------------------------*/

char	szRomName[256];
char	szSaveName[256];
int	nSRAM_SaveFlag;

/*-------------------------------------------------------------------*/
/*  Constants ( Linux specific )                                     */
/*-------------------------------------------------------------------*/

#define VBOX_SIZE	7
#define SOUND_DEVICE	"/dev/dsp"
#define VERSION		"InfoNES v0.91J"

/*-------------------------------------------------------------------*/
/*  Global Variables ( Linux specific )                              */
/*-------------------------------------------------------------------*/

/* Emulation thread */
pthread_t  emulation_tid = 0;
int bThread = FALSE;

/* Pad state */
DWORD	dwKeyPad1;
DWORD	dwKeyPad2;
DWORD	dwKeySystem;

/* For Sound Emulation */
BYTE	final_wave[2048];
int	waveptr;
int	wavflag;
int	sound_fd;

void *emulation_thread( void *args );
int LoadSRAM();
int SaveSRAM();


/* Palette data */
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

void *emulation_thread( void *args )
{
	InfoNES_Main();
    return NULL;
}

int LoadSRAM()
{

	FILE		*fp;
	unsigned char	pSrcBuf[SRAM_SIZE];
	unsigned char	chData;
	unsigned char	chTag;
	int		nRunLen;
	int		nDecoded;
	int		nDecLen;
	int		nIdx;

	/* It doesn't need to save it */
	nSRAM_SaveFlag = 0;

	/* It is finished if the ROM doesn't have SRAM */
	if ( !ROM_SRAM )
		return(0);

	/* There is necessity to save it */
	nSRAM_SaveFlag = 1;

	/* The preparation of the SRAM file name */
	strcpy( szSaveName, szRomName );
	strcpy( strrchr( szSaveName, '.' ) + 1, "srm" );

	/* Open SRAM file */
	fp = fopen( szSaveName, "rb" );
	if ( fp == NULL )
		return(-1);

	/* Read SRAM data */
	fread( pSrcBuf, SRAM_SIZE, 1, fp );

	/* Close SRAM file */
	fclose( fp );

	nDecoded	= 0;
	nDecLen		= 0;

	chTag = pSrcBuf[nDecoded++];

	while ( nDecLen < 8192 )
	{
		chData = pSrcBuf[nDecoded++];

		if ( chData == chTag )
		{
			chData	= pSrcBuf[nDecoded++];
			nRunLen = pSrcBuf[nDecoded++];
			for ( nIdx = 0; nIdx < nRunLen + 1; ++nIdx )
			{
				SRAM[nDecLen++] = chData;
			}
		}else  {
			SRAM[nDecLen++] = chData;
		}
	}

	/* Successful */
	return(0);
}

int SaveSRAM()
{
	FILE		*fp;
	int		nUsedTable[256];
	unsigned char	chData;
	unsigned char	chPrevData;
	unsigned char	chTag;
	int		nIdx;
	int		nEncoded;
	int		nEncLen;
	int		nRunLen;
	unsigned char	pDstBuf[SRAM_SIZE];

	if ( !nSRAM_SaveFlag )
		return(0);  /* It doesn't need to save it */

	memset( nUsedTable, 0, sizeof nUsedTable );

	for ( nIdx = 0; nIdx < SRAM_SIZE; ++nIdx )
	{
		++nUsedTable[SRAM[nIdx++]];
	}
	for ( nIdx = 1, chTag = 0; nIdx < 256; ++nIdx )
	{
		if ( nUsedTable[nIdx] < nUsedTable[chTag] )
			chTag = nIdx;
	}

	nEncoded	= 0;
	nEncLen		= 0;
	nRunLen		= 1;

	pDstBuf[nEncLen++] = chTag;

	chPrevData = SRAM[nEncoded++];

	while ( nEncoded < SRAM_SIZE && nEncLen < SRAM_SIZE - 133 )
	{
		chData = SRAM[nEncoded++];

		if ( chPrevData == chData && nRunLen < 256 )
			++nRunLen;
		else{
			if ( nRunLen >= 4 || chPrevData == chTag )
			{
				pDstBuf[nEncLen++]	= chTag;
				pDstBuf[nEncLen++]	= chPrevData;
				pDstBuf[nEncLen++]	= nRunLen - 1;
			}else  {
				for ( nIdx = 0; nIdx < nRunLen; ++nIdx )
					pDstBuf[nEncLen++] = chPrevData;
			}

			chPrevData	= chData;
			nRunLen		= 1;
		}
	}
	if ( nRunLen >= 4 || chPrevData == chTag )
	{
		pDstBuf[nEncLen++]	= chTag;
		pDstBuf[nEncLen++]	= chPrevData;
		pDstBuf[nEncLen++]	= nRunLen - 1;
	}else  {
		for ( nIdx = 0; nIdx < nRunLen; ++nIdx )
			pDstBuf[nEncLen++] = chPrevData;
	}

	/* Open SRAM file */
	fp = fopen( szSaveName, "wb" );
	if ( fp == NULL )
		return(-1);

	/* Write SRAM data */
	fwrite( pDstBuf, nEncLen, 1, fp );

	/* Close SRAM file */
	fclose( fp );

	/* Successful */
	return(0);
}

int InfoNES_Menu()
{
	if ( bThread == FALSE )
	{
		return(-1);
	}
	return(0);
}

int InfoNES_ReadRom( const char *pszFileName )
{
	FILE *fp;

	/* Open ROM file */
	fp = fopen( pszFileName, "rb" );
	if ( fp == NULL )
		return(-1);

	/* Read ROM Header */
	fread( &NesHeader, sizeof NesHeader, 1, fp );
	if ( memcmp( NesHeader.byID, "NES\x1a", 4 ) != 0 )
	{
		/* not .nes file */
		fclose( fp );
		return(-1);
	}

	/* Clear SRAM */
	memset( SRAM, 0, SRAM_SIZE );

	/* If trainer presents Read Triner at 0x7000-0x71ff */
	if ( NesHeader.byInfo1 & 4 )
	{
		fread( &SRAM[0x1000], 512, 1, fp );
	}

	/* Allocate Memory for ROM Image */
	ROM = (BYTE *) malloc( NesHeader.byRomSize * 0x4000 );

	/* Read ROM Image */
	fread( ROM, 0x4000, NesHeader.byRomSize, fp );

	if ( NesHeader.byVRomSize > 0 )
	{
		/* Allocate Memory for VROM Image */
		VROM = (BYTE *) malloc( NesHeader.byVRomSize * 0x2000 );

		/* Read VROM Image */
		fread( VROM, 0x2000, NesHeader.byVRomSize, fp );
	}

	/* File close */
	fclose( fp );

	/* Successful */
	return(0);
}

void InfoNES_ReleaseRom()
{
	if ( ROM )
	{
		free( ROM );
		ROM = NULL;
	}

	if ( VROM )
	{
		free( VROM );
		VROM = NULL;
	}
}

void *InfoNES_MemoryCopy( void *dest, const void *src, int count )
{
	memcpy( dest, src, count );
	return(dest);
}

void *InfoNES_MemorySet( void *dest, int c, int count )
{
	memset( dest, c, count );
	return(dest);
}

void InfoNES_LoadFrame()
{
	int x,y;
	int line_width;
	WORD wColor;

	//修正 即便没有 LCD 也可以出声
	if(0 < fb_fd)
	{
		for (y = 0; y < lcd_height; y++ )
		{
			line_width = zoom_y_tab[y] * NES_DISP_WIDTH;
			for (x = 0; x < lcd_width; x++ )
			{
				wColor = WorkFrame[line_width  + zoom_x_tab[x]];
				lcd_fb_display_px(wColor, x, y);
			}
		}
	}
}

void InfoNES_PadState( DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem )
{
	/* Transfer joypad state */
	*pdwPad1	= dwKeyPad1;
	*pdwPad2	= dwKeyPad2;
	*pdwSystem	= dwKeySystem;

	dwKeyPad1 = 0;
}

void InfoNES_SoundInit( void )
{
	
}

int InfoNES_SoundOpen( int samples_per_sync, int sample_rate )
{
	//sample_rate 采样率 44100
	//samples_per_sync  735
	unsigned int rate      = sample_rate;
	snd_pcm_hw_params_t *hw_params;
	return 0;
	if(0 > snd_pcm_open(&playback_handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) 
	{
		printf("snd_pcm_open err\n");
		return -1;
	}
	
	if(0 > snd_pcm_hw_params_malloc(&hw_params))
	{
		printf("snd_pcm_hw_params_malloc err\n");
		return -1;
	}
	
	if(0 > snd_pcm_hw_params_any(playback_handle, hw_params))
	{
		printf("snd_pcm_hw_params_any err\n");
		return -1;
	}
	if(0 > snd_pcm_hw_params_set_access(playback_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) 
	{
		printf("snd_pcm_hw_params_any err\n");
		return -1;
	}

	//8bit PCM 数据
	if(0 > snd_pcm_hw_params_set_format(playback_handle, hw_params, SND_PCM_FORMAT_U8))
	{
		printf("snd_pcm_hw_params_set_format err\n");
		return -1;
	}

	if(0 > snd_pcm_hw_params_set_rate_near(playback_handle, hw_params, &rate, 0)) 
	{
		printf("snd_pcm_hw_params_set_rate_near err\n");
		return -1;
	}

	//单声道 非立体声
	if(0 > snd_pcm_hw_params_set_channels(playback_handle, hw_params, 1))
	{
		printf("snd_pcm_hw_params_set_channels err\n");
		return -1;
	}

	if(0 > snd_pcm_hw_params(playback_handle, hw_params)) 
	{
		printf("snd_pcm_hw_params err\n");
		return -1;
	}
	
	snd_pcm_hw_params_free(hw_params);
	
	if(0 > snd_pcm_prepare(playback_handle)) 
	{
		printf("snd_pcm_prepare err\n");
		return -1;
	}
	return 1;
}

void InfoNES_SoundClose( void )
{
    return;
	snd_pcm_close(playback_handle);
}

void InfoNES_SoundOutput( int samples, BYTE *wave1, BYTE *wave2, BYTE *wave3, BYTE *wave4, BYTE *wave5 )
{
	int i;
	int ret;
	unsigned char wav;
	unsigned char *pcmBuf = (unsigned char *)malloc(samples);
	return;
    for (i=0; i <samples; i++)
	{
		wav = (wave1[i] + wave2[i] + wave3[i] + wave4[i] + wave5[i]) / 5;
		//单声道 8位数据
		pcmBuf[i] = wav;
	}
	ret = snd_pcm_writei(playback_handle, pcmBuf, samples);
	if(-EPIPE == ret)
    {
        snd_pcm_prepare(playback_handle);
    }
	free(pcmBuf);
	return ;
}

void InfoNES_Wait()
{
}

void InfoNES_MessageBox(const char *pszMsg, ... )
{
	printf( "MessageBox: %s \n", pszMsg );
}

extern "C" {

    static void core_init() {
        playback_handle = NULL;
        fb_mem = NULL;
        fb_fd = -1;
        emulation_tid = 0;
        zoom_x_tab = NULL;
        zoom_y_tab = NULL;
    }
    
    static void core_load_game(const char* path) {
        printf("[Core] Loading game: %s\n", path);
        
        AdapterInitGameInput();
        if (lcd_fb_init() != 0) {
             printf("[Core] LCD Init Failed\n");
             return;
        }
        
        if (make_zoom_tab() != 1) {
             printf("[Core] Zoom Tab Failed\n");
             return;
        }

        if (InfoNES_Load(path) == 0) {
            LoadSRAM();
            printf("[Core] Init success\n");
        } else {
            printf("[Core] Load failed\n");
        }

        InfoNES_Init();
        bThread = TRUE;
		pthread_create( &emulation_tid, NULL, emulation_thread, NULL );
    }

    static void core_run_frame(uint32_t* buffer) {
		dwKeyPad1 = AdapterGetGameInput();
		printf("KeyPad1: %u\n", dwKeyPad1);
    }

    static void core_input(uint32_t keys) {
    }

    static void core_cleanup() {
        printf("[Core] Begin cleaning up...\n");
        bThread = FALSE; 
        if (emulation_tid != 0) {
            pthread_join(emulation_tid, NULL);
            emulation_tid = 0;
        }

        if (fb_mem && fb_mem != (void*)-1) {
            munmap(fb_mem, var.yres * var.xres * (var.bits_per_pixel / 8));
            fb_mem = NULL;
        }

        if (fb_fd >= 0) {
            close(fb_fd);
            fb_fd = -1;
        }

        if (zoom_x_tab) { free(zoom_x_tab); zoom_x_tab = NULL; }
        if (zoom_y_tab) { free(zoom_y_tab); zoom_y_tab = NULL; }
        
        AdapterExitGameInput();
        
        printf("[Core] Cleaning up success\n");
    }

    NanoCore* get_core() {
        static NanoCore core = {
            "InfoNES (NES)",
            core_init,
            core_load_game,
            core_run_frame,
            core_input,
            core_cleanup
        };
        return &core;
    }
}