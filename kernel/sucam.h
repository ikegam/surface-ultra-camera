/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Surface Laptop Ultra cameras (MediaTek ISP8 seninf -> camsv):
 *   front: OV02C10, CSI port 0, 2 lanes
 *   IR:    VD55G1,  CSI port 2, 1 lane
 * Both use outmux1 -> camsv_b, so only one of them can stream at a time.
 */
#ifndef SUCAM_DRV_H
#define SUCAM_DRV_H

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#define SUCAM_W		1920
#define SUCAM_H		1080
#define SUCAM_STRIDE	2400			/* 1920 * 10 / 8, MTK packed RAW10 */
#define SUCAM_SIZE	(SUCAM_STRIDE * SUCAM_H)
#define SUCAM_FPS	30

/* camsv_b interrupts in ISP1 _CRS: 0x20A = done/err, 0x20D = SOF (tag n at bit 2+4n) */
#define SUCAM_SOF_GSI	0x20D

/* MediaTek ISP packed 10-bit GBRG (little-endian bitstream), same fourcc the
 * downstream MediaTek ISP driver uses for V4L2_PIX_FMT_MTISP_SGBRG10 */
#define SUCAM_FOURCC	v4l2_fourcc('M', 'B', 'G', 'A')

/*
 * OV02C10 2-lane 1928x1092 mode. The stock table (HTS 1140, VTS 2328) assumes a
 * 19.2 MHz MCLK and runs at 18.8 fps here (line time 22.8 us, i.e. the board
 * oscillator is ~12 MHz), so the frame length is shortened to get 30 fps.
 */
#define SUCAM_VTS		1462
#define SUCAM_EXP_MIN		4
#define SUCAM_EXP_MAX		(SUCAM_VTS - 8)
#define SUCAM_EXP_DEF		1132
#define SUCAM_AGAIN_MIN		0x10
#define SUCAM_AGAIN_MAX		0xf8
#define SUCAM_AGAIN_DEF		0x80
#define SUCAM_DGAIN_MIN		0x400
#define SUCAM_DGAIN_MAX		0x3fff
#define SUCAM_DGAIN_DEF		0x400

/* IR camera: 600x600 mono RAW10 (Windows' crop of the 804x704 sensor), MTK packed */
#define SUCAM_IR_W		600
#define SUCAM_IR_H		600
#define SUCAM_IR_STRIDE		752		/* 600 * 10 / 8 = 750, 16-byte aligned */
#define SUCAM_IR_FOURCC		v4l2_fourcc('M', 'Y', '1', '0')	/* MTK packed Y10 */
#define SUCAM_IR_FRAME_LENGTH	4010		/* lines: 30 fps at 1086 Mbps */
#define SUCAM_IR_EXP_MAX	(SUCAM_IR_FRAME_LENGTH - 64)
#define SUCAM_IR_EXP_DEF	1500
#define SUCAM_IR_AGAIN_MAX	28		/* gain = 32 / (32 - val) */
#define SUCAM_IR_AGAIN_DEF	16

struct sucam_cfg {
	bool ir;
	const char *card;		/* V4L2 card name */
	const char *input;
	const char *fmt_desc;
	u32 fourcc, width, height, stride;
};

extern const struct sucam_cfg sucam_front_cfg, sucam_ir_cfg;

enum { SUCAM_GPIO_CAMPOWER, SUCAM_GPIO_AVDD, SUCAM_GPIO_MCLK, SUCAM_GPIO_RESET,
       SUCAM_GPIO_PLED, SUCAM_GPIO_NUM };

struct sucam_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct sucam {
	struct device *dev;
	const struct sucam_cfg *cfg;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue queue;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure, *again, *dgain;
	struct mutex lock;		/* vdev / vb2 / hw state */

	/* hw */
	void __iomem *hfrp, *ccu, *sv;
	struct i2c_client *sensor;
	struct device *sensor_dev;
	struct gpio_desc *gpio[SUCAM_GPIO_NUM];
	bool powered, streaming;

	/* buffer flow (camsv FBC, driven by the camsv_b SOF interrupt) */
	spinlock_t qlock;
	struct list_head queued;	/* waiting to be armed */
	struct sucam_buf *pending;	/* in the outer WDMA addr, latched at next SOF */
	struct sucam_buf *writing;	/* latched by the last SOF, being written */
	int irq;			/* SOF interrupt, < 0 if unavailable */
	bool irq_on;			/* requested for the current stream */
	struct task_struct *poll;	/* fallback when there is no interrupt */
	struct delayed_work watchdog;	/* logs hardware state while frames stall */
	u32 watch_seq;
	u8 last_wcnt;
	u32 sequence;
	u32 dropped;		/* frames lost at SOF (buffer requeued) */
};

int sucam_hw_init(struct sucam *cam);
void sucam_hw_exit(struct sucam *cam);
int sucam_hw_start(struct sucam *cam, dma_addr_t first);
void sucam_hw_stop(struct sucam *cam);
void sucam_hw_arm(struct sucam *cam, dma_addr_t addr);
u8 sucam_hw_wcnt(struct sucam *cam);
bool sucam_hw_sof(struct sucam *cam);
void sucam_hw_diag(struct sucam *cam);
int sucam_sensor_set(struct sucam *cam, u32 id, s32 val);

/* sucam_vd55g1.c */
int sucam_vd_power(struct sucam *cam, bool on);
int sucam_vd_stream(struct sucam *cam, bool on);
int sucam_vd_set(struct sucam *cam, u32 id, s32 val);

#endif
