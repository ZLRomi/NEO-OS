#include "audio.h"
#include "pci.h"
#include "memory.h"
#include "io.h"
#include "printk.h"
#include "serial.h"
#include <stdint.h>

#define AC97_VENDOR 0x8086
#define AC97_DEVICE 0x2415

#define AC97_PCM_FRONT_DAC_RATE 0x2C

#define NAM_REGISTER_READ   0x00
#define NAM_REGISTER_WRITE  0x04
#define NAM_STATUS          0x06
#define NAM_CONTROL         0x08

#define PIOC_BUFFER_BASE 0x10
#define PIOC_LAST_INDEX  0x15
#define PIOC_FRONT_SR    0x16
#define PIOC_LVI         0x17

#define GLOB_CNT   0x2C
#define GLOB_STA   0x30

#define BDL_ENTRIES 8

struct bdl_entry {
    uint32_t addr;
    uint32_t len_ctrl;
} __attribute__((packed));

static int ac97_present = 0;
static uint16_t nam_io = 0;
static uint32_t nabmbar_io = 0;
static int master_volume = 75;

static struct bdl_entry* bdl = 0;
static uint32_t bdl_phys = 0;
static uint8_t* pcm_buf[AUDIO_NUM_BUFFERS];
static uint32_t pcm_buf_phys[AUDIO_NUM_BUFFERS];

static audio_stream_t streams[AUDIO_MAX_STREAMS];
static int last_buffer_idx = 0;

static uint16_t ac97_nam_read(uint8_t reg) {
    outw(nam_io + NAM_REGISTER_READ, reg);
    for (volatile int i = 0; i < 100; i++);
    return inw(nam_io + NAM_REGISTER_READ + 2);
}

static void ac97_nam_write(uint8_t reg, uint16_t val) {
    outw(nam_io + NAM_REGISTER_WRITE, reg);
    outw(nam_io + NAM_REGISTER_WRITE + 2, val);
}

static void ac97_pioc_write(uint16_t reg, uint32_t val) {
    if (reg == PIOC_BUFFER_BASE) {
        outl(nabmbar_io + reg, val);
    } else if (reg == PIOC_LAST_INDEX || reg == PIOC_LVI) {
        outb(nabmbar_io + reg, (uint8_t)val);
    } else if (reg <= 0xFF) {
        outb(nabmbar_io + reg, (uint8_t)val);
    } else {
        outw(nabmbar_io + reg, (uint16_t)val);
    }
}

static uint8_t ac97_pioc_read8(uint16_t reg) {
    return inb(nabmbar_io + reg);
}

static void ac97_reset(void) {
    outl(nabmbar_io + GLOB_CNT, 0);
    for (volatile int i = 0; i < 1000; i++);
    outl(nabmbar_io + GLOB_CNT, 0x02);
    for (volatile int i = 0; i < 50000; i++) {
        if (inl(nabmbar_io + GLOB_STA) & 0x10) break;
    }
    outl(nabmbar_io + GLOB_CNT, 0x04);
    for (volatile int i = 0; i < 50000; i++);
}

void audio_init(void) {
    pci_dev_t dev;
    if (!pci_find_device(AC97_VENDOR, AC97_DEVICE, &dev)) {
        serial_write("AC97: device not found\n");
        ac97_present = 0;
        return;
    }
    pci_enable_bus_master(&dev);

    nam_io = (uint16_t)(dev.bar[0] & 0xFFFE);
    nabmbar_io = dev.bar[1] & 0xFFFFFFF0;
    if (!nam_io || !nabmbar_io) {
        serial_write("AC97: BAR not configured\n");
        ac97_present = 0;
        return;
    }
    if (nam_io == 0xC000 || nabmbar_io == 0xC000) {
        nam_io = 0x0800;
        nabmbar_io = 0x0C00;
    }

    ac97_present = 1;
    ac97_reset();

    uint16_t ext_id = ac97_nam_read(0x28);
    (void)ext_id;

    ac97_nam_write(0x02, 0x0000);
    ac97_nam_write(0x18, 0x0000);
    ac97_nam_write(0x04, 0x0000);
    ac97_nam_write(0x06, 0x0000);

    ac97_nam_write(AC97_PCM_FRONT_DAC_RATE, AUDIO_SAMPLE_RATE);
    for (volatile int i = 0; i < 5000; i++);

    bdl = (struct bdl_entry*)dma_alloc_page(&bdl_phys);
    if (!bdl) {
        serial_write("AC97: BDL alloc failed\n");
        ac97_present = 0;
        return;
    }
    for (int i = 0; i < BDL_ENTRIES; i++) {
        bdl[i].addr = 0;
        bdl[i].len_ctrl = 0;
    }

    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        pcm_buf[i] = (uint8_t*)dma_alloc_page(&pcm_buf_phys[i]);
        if (!pcm_buf[i]) {
            serial_write("AC97: buffer alloc failed\n");
            ac97_present = 0;
            return;
        }
        for (int j = 0; j < AUDIO_BUFFER_BYTES; j++) pcm_buf[i][j] = 0;
    }

    ac97_pioc_write(PIOC_BUFFER_BASE, bdl_phys);
    ac97_pioc_write(0x1B, 0x00);

    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        streams[i].used = 0;
        streams[i].state = AUDIO_STATE_IDLE;
    }

    audio_set_master_volume(master_volume);
    printk("AC97: initialized nam_io=0x%x nabmbar=0x%x rate=%d\n",
           nam_io, nabmbar_io, AUDIO_SAMPLE_RATE);
    serial_write("AC97: initialized\n");
}

int audio_is_present(void) {
    return ac97_present;
}

void audio_set_master_volume(int volume) {
    if (!ac97_present) return;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    master_volume = volume;
    uint16_t v = (uint16_t)((100 - volume) * 63 / 100);
    uint16_t reg_val = (v << 8) | v;
    ac97_nam_write(0x02, reg_val);
}

int audio_get_volume(void) {
    return master_volume;
}

static int alloc_stream(void) {
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        if (!streams[i].used) return i;
    }
    return -1;
}

int audio_play(const void* pcm_data, uint32_t bytes, uint32_t sample_rate,
               int channels, int loop, void (*on_complete)(void*), void* ctx) {
    if (!ac97_present || !pcm_data || bytes == 0) return -1;
    int idx = alloc_stream();
    if (idx < 0) return -1;
    audio_stream_t* s = &streams[idx];
    s->used = 1;
    s->state = AUDIO_STATE_PLAYING;
    s->sample_rate = sample_rate ? sample_rate : AUDIO_SAMPLE_RATE;
    s->channels = channels ? channels : AUDIO_CHANNELS;
    s->pcm_data = pcm_data;
    s->pcm_total_bytes = bytes;
    s->pcm_pos = 0;
    s->loop = loop;
    s->on_complete = 0;
    s->ctx = ctx;
    (void)on_complete;

    if (sample_rate && sample_rate != AUDIO_SAMPLE_RATE) {
        ac97_nam_write(AC97_PCM_FRONT_DAC_RATE, (uint16_t)sample_rate);
        for (volatile int i = 0; i < 5000; i++);
    }
    return idx;
}

void audio_stop(int handle) {
    if (handle < 0 || handle >= AUDIO_MAX_STREAMS) return;
    audio_stream_t* s = &streams[handle];
    if (!s->used) return;
    s->state = AUDIO_STATE_STOPPED;
    s->used = 0;
}

void audio_pause(int handle) {
    if (handle < 0 || handle >= AUDIO_MAX_STREAMS) return;
    audio_stream_t* s = &streams[handle];
    if (s->used && s->state == AUDIO_STATE_PLAYING) {
        s->state = AUDIO_STATE_PAUSED;
    }
}

void audio_resume(int handle) {
    if (handle < 0 || handle >= AUDIO_MAX_STREAMS) return;
    audio_stream_t* s = &streams[handle];
    if (s->used && s->state == AUDIO_STATE_PAUSED) {
        s->state = AUDIO_STATE_PLAYING;
    }
}

void audio_set_volume(int handle, int volume) {
    (void)handle;
    audio_set_master_volume(volume);
}

uint32_t audio_get_position_ms(int handle) {
    if (handle < 0 || handle >= AUDIO_MAX_STREAMS) return 0;
    audio_stream_t* s = &streams[handle];
    if (!s->used) return 0;
    uint32_t bytes = s->pcm_pos;
    uint32_t frames = bytes / AUDIO_FRAME_BYTES;
    if (s->sample_rate == 0) return 0;
    return frames * 1000 / s->sample_rate;
}

uint32_t audio_get_duration_ms(int handle) {
    if (handle < 0 || handle >= AUDIO_MAX_STREAMS) return 0;
    audio_stream_t* s = &streams[handle];
    if (!s->used || s->sample_rate == 0) return 0;
    uint32_t frames = s->pcm_total_bytes / AUDIO_FRAME_BYTES;
    return frames * 1000 / s->sample_rate;
}

static void fill_buffer(uint8_t* dst, audio_stream_t* s, uint32_t need) {
    uint32_t filled = 0;
    while (filled < need) {
        uint32_t remain = s->pcm_total_bytes - s->pcm_pos;
        if (remain == 0) {
            if (s->loop) {
                s->pcm_pos = 0;
                remain = s->pcm_total_bytes;
            } else {
                for (uint32_t i = filled; i < need; i++) dst[i] = 0;
                break;
            }
        }
        uint32_t copy = need - filled;
        if (copy > remain) copy = remain;
        const uint8_t* src = (const uint8_t*)s->pcm_data + s->pcm_pos;
        for (uint32_t i = 0; i < copy; i++) dst[filled + i] = src[i];
        s->pcm_pos += copy;
        filled += copy;
        if (s->pcm_pos >= s->pcm_total_bytes) {
            if (s->loop) s->pcm_pos = 0;
            else break;
        }
    }
}

void audio_poll(void) {
    if (!ac97_present) return;

    audio_stream_t* active = 0;
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        if (streams[i].used && streams[i].state == AUDIO_STATE_PLAYING) {
            active = &streams[i];
            break;
        }
    }
    if (!active) {
        uint8_t sr = ac97_pioc_read8(0x16);
        if (sr & 0x02) {
            ac97_pioc_write(0x1B, 0x00);
        }
        return;
    }

    uint8_t status = ac97_pioc_read8(0x16);
    int next = (last_buffer_idx + 1) % AUDIO_NUM_BUFFERS;
    if (status & 0x02) {
        fill_buffer(pcm_buf[next], active, AUDIO_BUFFER_BYTES);
        bdl[next % BDL_ENTRIES].addr = pcm_buf_phys[next];
        bdl[next % BDL_ENTRIES].len_ctrl = AUDIO_BUFFER_BYTES | 0x80000000;
        ac97_pioc_write(0x15, next % BDL_ENTRIES);
        ac97_pioc_write(0x17, next % BDL_ENTRIES);
        last_buffer_idx = next;
        ac97_pioc_write(0x1B, 0x01);
    } else if (!(status & 0x01)) {
        fill_buffer(pcm_buf[0], active, AUDIO_BUFFER_BYTES);
        bdl[0].addr = pcm_buf_phys[0];
        bdl[0].len_ctrl = AUDIO_BUFFER_BYTES | 0x80000000;
        for (int i = 1; i < BDL_ENTRIES; i++) {
            bdl[i].addr = pcm_buf_phys[i % AUDIO_NUM_BUFFERS];
            bdl[i].len_ctrl = AUDIO_BUFFER_BYTES | 0x80000000;
        }
        ac97_pioc_write(0x15, 0);
        ac97_pioc_write(0x17, BDL_ENTRIES - 1);
        ac97_pioc_write(0x1B, 0x01);
        last_buffer_idx = 0;
    }

    if (active->pcm_pos >= active->pcm_total_bytes && !active->loop) {
        active->state = AUDIO_STATE_STOPPED;
        active->used = 0;
    }
}
