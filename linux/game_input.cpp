#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <linux/input.h>
#include <stdlib.h>

// keyboard input device
#define JOYPAD_DEV "/dev/input/event3"
// USB joystick device
#define USB_JS_DEV "/dev/input/js0"
// touch screen device
#define TOUCH_DEV "/dev/input/event0"

typedef struct JoypadInput{
	int (*DevInit)(void);
	int (*DevExit)(void);
	int (*GetJoypad)(void);
	struct JoypadInput *ptNext;
	pthread_t tTreadID;
}T_JoypadInput, *PT_JoypadInput;

struct js_event {		
	unsigned int   time;      /* event timestamp in milliseconds */		
	unsigned short value;     /* value */		
	unsigned char  type;      /* event type */		
	unsigned char  number;    /* axis/button number */	
};

static unsigned char g_InputEvent;

static pthread_mutex_t g_tMutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_tConVar = PTHREAD_COND_INITIALIZER;

static int joypad_fd;
static int USBjoypad_fd;
static int touch_fd;
static PT_JoypadInput g_ptJoypadInputHead;

#define MAX_SLOTS 2 // two fingers supported
#define THRESHOLD 20

// touch screen
typedef struct {
    int x, y;
    int active;
    int is_rocker;
    int center_x;
    int center_y;
} T_TouchSlot;

static T_TouchSlot slots[MAX_SLOTS];
static int cur_slot = 0;

// input event thread function
static void *InputEventTreadFunction(void *pVoid)
{
	int (*GetJoypad)(void);
	GetJoypad = (int (*)(void))pVoid;

	while (1)
	{
		g_InputEvent = GetJoypad();

		pthread_mutex_lock(&g_tMutex);
		pthread_cond_signal(&g_tConVar);
		pthread_mutex_unlock(&g_tMutex);
	}
}

// event create
static int RegisterGameInput(PT_JoypadInput ptJoypadInput)
{
	PT_JoypadInput tmp;
	if(ptJoypadInput->DevInit())
	{
		return -1;
	}

	pthread_create(&ptJoypadInput->tTreadID, NULL, InputEventTreadFunction, (void*)ptJoypadInput->GetJoypad);
	if(! g_ptJoypadInputHead)
	{
		g_ptJoypadInputHead = ptJoypadInput;
	}
	else
	{
		tmp = g_ptJoypadInputHead;
		while(tmp->ptNext)
		{
			tmp = tmp->ptNext;
		}
		tmp->ptNext = ptJoypadInput;
	}
	ptJoypadInput->ptNext = NULL;
	return 0;
}

/*==============================*/
/*     Touch Screen Input       */
/*==============================*/
static int TouchGet(void)
{
    struct input_event ev;
    static unsigned char last_joypad = 0;

    while (read(touch_fd, &ev, sizeof(ev)) > 0) {
        if (ev.type == EV_ABS) {
            switch (ev.code) {
                case ABS_MT_SLOT:
                    if (ev.value < MAX_SLOTS) cur_slot = ev.value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (ev.value == -1) {
                        slots[cur_slot].active = 0;
                        slots[cur_slot].is_rocker = -1;
                    } else { 
                        slots[cur_slot].active = 1;
                        slots[cur_slot].is_rocker = -1;
                    }
                    break;
                case ABS_MT_POSITION_X:
                    slots[cur_slot].x = ev.value;
                    break;
                case ABS_MT_POSITION_Y:
                    slots[cur_slot].y = ev.value;
                    break;
            }
        }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            unsigned char current_val = 0;

            for (int i = 0; i < MAX_SLOTS; i++) {
                if (!slots[i].active) continue;

                if (slots[i].is_rocker == -1) {
                    if (slots[i].x < 240) {
                        slots[i].is_rocker = 1;
                        slots[i].center_x = slots[i].x;
                        slots[i].center_y = slots[i].y;
                    } else {
                        slots[i].is_rocker = 0;
                    }
                }

                if (slots[i].is_rocker == 1) {
                    int dx = slots[i].x - slots[i].center_x;
                    int dy = slots[i].y - slots[i].center_y;

                    if (dy < -THRESHOLD) current_val |= (1 << 4); // up
                    if (dy > THRESHOLD)  current_val |= (1 << 5); // down
                    if (dx < -THRESHOLD) current_val |= (1 << 6); // left
                    if (dx > THRESHOLD)  current_val |= (1 << 7); // right
                } else if (slots[i].is_rocker == 0) {
                    int tx = slots[i].x;
                    int ty = slots[i].y;
                    if (ty > 400) {
                        if (tx > 380) current_val |= (1 << 0); // A
                        else          current_val |= (1 << 1); // B
                    } else if (ty < 200) {
                        if (tx > 380) current_val |= (1 << 3); // start
                        else          current_val |= (1 << 2); // select
                    }
                }
            }

            if (current_val != last_joypad) {
                last_joypad = current_val;
                return (int)current_val;
            }
        }
    }
    return -1; 
}

static int TouchDevInit(void)
{
    touch_fd = open(TOUCH_DEV, O_RDONLY);
    if(-1 == touch_fd)
    {
        printf("%s dev not found \r\n", TOUCH_DEV);
        return -1;
    }
    return 0;
}

static int TouchDevExit(void)
{
    if (touch_fd >= 0) close(touch_fd);
    touch_fd = -1;
    return 0;
}

static T_JoypadInput touchInput = {
    TouchDevInit,
    TouchDevExit,
    TouchGet,
};

/*==============================*/
/*       keyboard Input         */
/*==============================*/
static int joypadGet(void)
{
#define KEY_UP 103
#define KEY_DOWN 108
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_A 44
#define KEY_B 45
#define KEY_ENTER 28
#define KEY_ESC 1
    struct input_event ev;
    static unsigned char joypad_status = 0;

    if (read(joypad_fd, &ev, sizeof(struct input_event)) > 0)
    {
        if (ev.type == EV_KEY)
        {
			// debug
            int pressed = (ev.value != 0); 

            switch (ev.code)
            {
                case KEY_UP:    if(pressed) joypad_status |= (1<<4); else joypad_status &= ~(1<<4); break;
                case KEY_DOWN:  if(pressed) joypad_status |= (1<<5); else joypad_status &= ~(1<<5); break;
                case KEY_LEFT:  if(pressed) joypad_status |= (1<<6); else joypad_status &= ~(1<<6); break;
                case KEY_RIGHT: if(pressed) joypad_status |= (1<<7); else joypad_status &= ~(1<<7); break;
                case KEY_A:     if(pressed) joypad_status |= (1<<0); else joypad_status &= ~(1<<0); break;
                case KEY_B:     if(pressed) joypad_status |= (1<<1); else joypad_status &= ~(1<<1); break;
                case KEY_ENTER: if(pressed) joypad_status |= (1<<3); else joypad_status &= ~(1<<3); break; // start
                case KEY_ESC:   if(pressed) joypad_status |= (1<<2); else joypad_status &= ~(1<<2); break; // select
				default: return -1;
            }
            return (int)joypad_status;
        }
    }
    return -1;
}

static int joypadDevInit(void)
{
	joypad_fd = open(JOYPAD_DEV, O_RDONLY);
	if(-1 == joypad_fd)
	{
		printf("%s dev not found \r\n", JOYPAD_DEV);
		return -1;
	}
	return 0;
}

static int joypadDevExit(void)
{
	close(joypad_fd);
	return 0;
}

static T_JoypadInput joypadInput = {
	joypadDevInit,
	joypadDevExit,
	joypadGet,
};

/*==============================*/
/*     USB Joystick Input       */
/*==============================*/
static int USBjoypadGet(void)
{
	static unsigned char joypad = 0;
	struct js_event e;
	if(0 < read (USBjoypad_fd, &e, sizeof(e)))
	{
		if(0x2 == e.type)
		{
			if(0x8001 == e.value && 0x5 == e.number)
			{
				joypad |= 1<<4;
			}

			if(0x7fff == e.value && 0x5 == e.number)
			{
				joypad |= 1<<5;
			}

			if(0x0 == e.value && 0x5 == e.number)
			{
				joypad &= ~(1<<4 | 1<<5);
			}
			
			if(0x8001 == e.value && 0x4 == e.number)
			{
				joypad |= 1<<6;
			}
			
			if(0x7fff == e.value && 0x4 == e.number)
			{
				joypad |= 1<<7;
			}

			if(0x0 == e.value && 0x4 == e.number)
			{
				joypad &= ~(1<<6 | 1<<7);
			}
		}

		if(0x1 == e.type)
		{
			if(0x1 == e.value && 0xa == e.number)
			{
				joypad |= 1<<2;
			}
			if(0x0 == e.value && 0xa == e.number)
			{
				joypad &= ~(1<<2);
			}

			if(0x1 == e.value && 0xb == e.number)
			{
				joypad |= 1<<3;
			}
			if(0x0 == e.value && 0xb == e.number)
			{
				joypad &= ~(1<<3);
			}

			if(0x1 == e.value && 0x0 == e.number)
			{
				joypad |= 1<<0;
			}
			if(0x0 == e.value && 0x0 == e.number)
			{
				joypad &= ~(1<<0);
			}

			if(0x1 == e.value && 0x1 == e.number)
			{
				joypad |= 1<<1;
			}
			if(0x0 == e.value && 0x1 == e.number)
			{
				joypad &= ~(1<<1);
			}

			if(0x1 == e.value && 0x3 == e.number)
			{
				joypad |= 1<<0;
			}
			if(0x0 == e.value && 0x3 == e.number)
			{
				joypad &= ~(1<<0);
			}

		 	if(0x1 == e.value && 0x4 == e.number)
			{
				joypad |= 1<<1;
			}
			if(0x0 == e.value && 0x4 == e.number)
			{
				joypad &= ~(1<<1);
			}
		}
		return joypad;
	}
	return -1;
}

static int USBjoypadDevInit(void)
{
	USBjoypad_fd = open(USB_JS_DEV, O_RDONLY);
	if(-1 == USBjoypad_fd)
	{
		printf("%s dev not found \r\n", USB_JS_DEV);
		return -1;
	}
	return 0;
}

static int USBjoypadDevExit(void)
{
	close(USBjoypad_fd);
	return 0;
}

static T_JoypadInput usbJoypadInput = {
	USBjoypadDevInit,
	USBjoypadDevExit,
	USBjoypadGet,
};

/*==============================*/
/*       Init Game Input        */
/*==============================*/
int InitGameInput(void)
{
	int iErr = 0;
	iErr = RegisterGameInput(&joypadInput);
	iErr = RegisterGameInput(&usbJoypadInput);
	iErr = RegisterGameInput(&touchInput);
	return iErr;
}

int GetGameInput(void)
{
	pthread_mutex_lock(&g_tMutex);
	pthread_cond_wait(&g_tConVar, &g_tMutex);	
	pthread_mutex_unlock(&g_tMutex);
	return g_InputEvent;
}

int ExitGameInput(void)
{
	joypadInput.DevExit();
	usbJoypadInput.DevExit();
	touchInput.DevExit();
	return 0;
}

/*==============================*/
/*     Adapter Game Input       */
/*==============================*/
static int adapter_joypad_fd = -1;
static unsigned int g_CurrentJoypadState = 0;

int AdapterInitGameInput(void)
{
    adapter_joypad_fd = open("/dev/input/event3", O_RDONLY | O_NONBLOCK);
    if(adapter_joypad_fd == -1)
    {
        printf("Error opening adapter joypad device\n");
        return -1;
    }
    g_CurrentJoypadState = 0;
    return 0;
}

int AdapterGetGameInput(void)
{
    if (adapter_joypad_fd != -1) {
		struct input_event ev;
		while (read(adapter_joypad_fd, &ev, sizeof(struct input_event)) > 0)
		{
			printf("[DEBUG] Event Received: Code=%d, Value=%d\n", ev.code, ev.value);
			if (ev.type == EV_KEY)
			{
				int pressed = (ev.value != 0);
				switch (ev.code)
				{
					case 103: /* UP */    if(pressed) g_CurrentJoypadState |= (1<<4); else g_CurrentJoypadState &= ~(1<<4); break;
					case 108: /* DOWN */  if(pressed) g_CurrentJoypadState |= (1<<5); else g_CurrentJoypadState &= ~(1<<5); break;
					case 105: /* LEFT */  if(pressed) g_CurrentJoypadState |= (1<<6); else g_CurrentJoypadState &= ~(1<<6); break;
					case 106: /* RIGHT */ if(pressed) g_CurrentJoypadState |= (1<<7); else g_CurrentJoypadState &= ~(1<<7); break;
					case 44:  /* Z/A */   if(pressed) g_CurrentJoypadState |= (1<<0); else g_CurrentJoypadState &= ~(1<<0); break;
					case 45:  /* X/B */   if(pressed) g_CurrentJoypadState |= (1<<1); else g_CurrentJoypadState &= ~(1<<1); break;
					case 28:  /* Enter */ if(pressed) g_CurrentJoypadState |= (1<<3); else g_CurrentJoypadState &= ~(1<<3); break; // Start
					case 1:   /* Esc */   if(pressed) g_CurrentJoypadState |= (1<<2); else g_CurrentJoypadState &= ~(1<<2); break; // Select
				}
			}
		}
    }
    return g_CurrentJoypadState;
}

int AdapterExitGameInput(void)
{
    if(adapter_joypad_fd != -1) {
        close(adapter_joypad_fd);
        adapter_joypad_fd = -1;
    }
    return 0;
}