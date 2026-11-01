// SPDX-License-Identifier: GPL-2.0
/*
 * Surface Laptop Ultra front camera: V4L2 capture device.
 *
 * Exposes the OV02C10 raw stream (1920x1080, MediaTek-packed 10-bit GBRG) as a
 * single video node. The sensor (and its privacy LED) is powered only while
 * streaming. Requires surface_ccu_boot to have booted the CCU firmware.
 */
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>
#include "sucam.h"

#define DRV_NAME	"surface-ultra-cam"

static struct platform_device *sucam_pdev;

static inline struct sucam_buf *to_sucam_buf(struct vb2_v4l2_buffer *vb)
{
	return container_of(vb, struct sucam_buf, vb);
}

static dma_addr_t buf_addr(struct sucam_buf *b)
{
	return vb2_dma_contig_plane_dma_addr(&b->vb.vb2_buf, 0);
}

/* ---------- frame completion (camsv FBC write count) ---------- */
static bool poll;
module_param(poll, bool, 0644);
MODULE_PARM_DESC(poll, "poll the camsv every 1 ms instead of using its SOF interrupt");

/*
 * Two buffers in flight: at each SOF the outer address (pending) is latched
 * and becomes the one being written, and we immediately program the next one,
 * so it has a whole frame time to land. A frame completes when the FBC write
 * count moves, which happens before the next SOF.
 */
static void sucam_frame_done(struct sucam *cam)
{
	struct sucam_buf *done;
	unsigned long flags;
	u8 w = sucam_hw_wcnt(cam);

	if (w == cam->last_wcnt)
		return;
	cam->last_wcnt = w;
	spin_lock_irqsave(&cam->qlock, flags);
	done = cam->writing;
	cam->writing = NULL;
	spin_unlock_irqrestore(&cam->qlock, flags);
	if (done) {
		done->vb.vb2_buf.timestamp = ktime_get_ns();
		done->vb.sequence = cam->sequence++;
		done->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&done->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
}

static void sucam_frame_start(struct sucam *cam)
{
	struct sucam_buf *next;
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	if (cam->pending) {
		cam->writing = cam->pending;
		cam->pending = NULL;
	}
	next = list_first_entry_or_null(&cam->queued, struct sucam_buf, list);
	if (next) {
		list_del(&next->list);
		cam->pending = next;
		sucam_hw_arm(cam, buf_addr(next));
	}
	spin_unlock_irqrestore(&cam->qlock, flags);
}

/* only requested while streaming: the camsv is powered and clocked then */
static irqreturn_t sucam_sof_irq(int irq, void *data)
{
	struct sucam *cam = data;

	if (!sucam_hw_sof(cam))		/* read-to-clear; the line is shared */
		return IRQ_NONE;
	sucam_frame_done(cam);
	sucam_frame_start(cam);
	return IRQ_HANDLED;
}

static int sucam_poll_fn(void *data)
{
	struct sucam *cam = data;

	while (!kthread_should_stop()) {
		sucam_frame_done(cam);
		if (sucam_hw_sof(cam))
			sucam_frame_start(cam);
		usleep_range(1000, 1500);
	}
	return 0;
}

static void sucam_watchdog(struct work_struct *work)
{
	struct sucam *cam = container_of(work, struct sucam, watchdog.work);

	if (cam->sequence == cam->watch_seq)	/* no frame for 1 s */
		sucam_hw_diag(cam);
	cam->watch_seq = cam->sequence;
	schedule_delayed_work(&cam->watchdog, HZ);
}

/* ---------- vb2 ---------- */
static int sucam_queue_setup(struct vb2_queue *q, unsigned int *nbuf, unsigned int *nplanes,
			     unsigned int sizes[], struct device *alloc_devs[])
{
	if (*nplanes)
		return sizes[0] < SUCAM_SIZE ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = SUCAM_SIZE;
	return 0;
}

static int sucam_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < SUCAM_SIZE)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, SUCAM_SIZE);
	return 0;
}

static void sucam_buf_queue(struct vb2_buffer *vb)
{
	struct sucam *cam = vb2_get_drv_priv(vb->vb2_queue);
	struct sucam_buf *b = to_sucam_buf(to_vb2_v4l2_buffer(vb));
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	if (cam->streaming && !cam->pending) {
		/* nothing waiting for the next SOF: program this one now (the outer
		 * address only takes effect at the next frame start) */
		cam->pending = b;
		sucam_hw_arm(cam, buf_addr(b));
	} else {
		list_add_tail(&b->list, &cam->queued);
	}
	spin_unlock_irqrestore(&cam->qlock, flags);
}

static void sucam_return_all(struct sucam *cam, enum vb2_buffer_state state)
{
	struct sucam_buf *b, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&cam->qlock, flags);
	if (cam->writing) {
		vb2_buffer_done(&cam->writing->vb.vb2_buf, state);
		cam->writing = NULL;
	}
	if (cam->pending) {
		vb2_buffer_done(&cam->pending->vb.vb2_buf, state);
		cam->pending = NULL;
	}
	list_for_each_entry_safe(b, tmp, &cam->queued, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&cam->qlock, flags);
}

static int sucam_hw_run(struct sucam *cam)
{
	struct sucam_buf *first;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&cam->qlock, flags);
	/* (re)start: anything in flight goes back to the head of the queue */
	if (cam->pending)
		list_add(&cam->pending->list, &cam->queued);
	if (cam->writing)
		list_add(&cam->writing->list, &cam->queued);
	cam->pending = cam->writing = NULL;
	first = list_first_entry_or_null(&cam->queued, struct sucam_buf, list);
	if (first)
		list_del(&first->list);
	cam->pending = first;
	spin_unlock_irqrestore(&cam->qlock, flags);
	if (!first)
		return -ENOBUFS;

	ret = sucam_hw_start(cam, buf_addr(first));
	if (ret)
		return ret;

	/*
	 * hw_start waits for data to reach outmux1, so the first buffer may
	 * already hold a frame: hand it out and arm the next one, so the SOF
	 * handler starts in step with the hardware.
	 */
	if (sucam_hw_wcnt(cam) != cam->last_wcnt) {
		struct sucam_buf *next;

		cam->last_wcnt = sucam_hw_wcnt(cam);
		sucam_hw_sof(cam);	/* drop the SOF that belonged to it */
		spin_lock_irqsave(&cam->qlock, flags);
		first = cam->pending;
		next = list_first_entry_or_null(&cam->queued, struct sucam_buf, list);
		if (next) {
			list_del(&next->list);
			sucam_hw_arm(cam, buf_addr(next));
		}
		cam->pending = next;
		spin_unlock_irqrestore(&cam->qlock, flags);
		first->vb.vb2_buf.timestamp = ktime_get_ns();
		first->vb.sequence = cam->sequence++;
		first->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&first->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}
	cam->irq_on = false;
	if (!poll && cam->irq >= 0) {
		ret = request_irq(cam->irq, sucam_sof_irq, IRQF_SHARED, DRV_NAME, cam);
		if (ret)
			dev_warn(cam->dev, "SOF irq %d: %d, polling instead\n", cam->irq, ret);
		cam->irq_on = !ret;
	}
	if (!cam->irq_on) {
		cam->poll = kthread_run(sucam_poll_fn, cam, "sucam-poll");
		if (IS_ERR(cam->poll)) {
			ret = PTR_ERR(cam->poll);
			cam->poll = NULL;
			sucam_hw_stop(cam);
			return ret;
		}
	}
	cam->streaming = true;
	cam->watch_seq = cam->sequence;
	schedule_delayed_work(&cam->watchdog, HZ);
	return 0;
}

static void sucam_hw_halt(struct sucam *cam)
{
	if (!cam->streaming)
		return;
	cancel_delayed_work_sync(&cam->watchdog);
	if (cam->irq_on)
		free_irq(cam->irq, cam);
	else
		kthread_stop(cam->poll);
	cam->irq_on = false;
	cam->poll = NULL;
	cam->streaming = false;
	sucam_hw_stop(cam);
}

static int sucam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sucam *cam = vb2_get_drv_priv(q);
	int ret;

	cam->sequence = 0;
	ret = sucam_hw_run(cam);
	if (ret)
		sucam_return_all(cam, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void sucam_stop_streaming(struct vb2_queue *q)
{
	struct sucam *cam = vb2_get_drv_priv(q);

	sucam_hw_halt(cam);
	sucam_return_all(cam, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops sucam_vb2_ops = {
	.queue_setup = sucam_queue_setup,
	.buf_prepare = sucam_buf_prepare,
	.buf_queue = sucam_buf_queue,
	.start_streaming = sucam_start_streaming,
	.stop_streaming = sucam_stop_streaming,
};

/* ---------- controls ---------- */
static int sucam_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sucam *cam = container_of(ctrl->handler, struct sucam, ctrls);

	return sucam_sensor_set(cam, ctrl->id, ctrl->val);
}

static const struct v4l2_ctrl_ops sucam_ctrl_ops = {
	.s_ctrl = sucam_s_ctrl,
};

/* ---------- ioctls ---------- */
static void sucam_fill_fmt(struct v4l2_pix_format *pix)
{
	pix->width = SUCAM_W;
	pix->height = SUCAM_H;
	pix->pixelformat = SUCAM_FOURCC;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = SUCAM_STRIDE;
	pix->sizeimage = SUCAM_SIZE;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->xfer_func = V4L2_XFER_FUNC_NONE;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
}

static int sucam_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Surface Laptop Ultra Camera", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" DRV_NAME, sizeof(cap->bus_info));
	return 0;
}

static int sucam_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = SUCAM_FOURCC;
	strscpy(f->description, "MTK packed 10-bit GBRG", sizeof(f->description));
	return 0;
}

static int sucam_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	sucam_fill_fmt(&f->fmt.pix);
	return 0;
}

static int sucam_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct sucam *cam = video_drvdata(file);

	if (vb2_is_busy(&cam->queue))
		return -EBUSY;
	sucam_fill_fmt(&f->fmt.pix);
	return 0;
}

static int sucam_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fs)
{
	if (fs->index || fs->pixel_format != SUCAM_FOURCC)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = SUCAM_W;
	fs->discrete.height = SUCAM_H;
	return 0;
}

static int sucam_enum_frameintervals(struct file *file, void *fh,
				     struct v4l2_frmivalenum *fi)
{
	if (fi->index || fi->pixel_format != SUCAM_FOURCC ||
	    fi->width != SUCAM_W || fi->height != SUCAM_H)
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fi->discrete.numerator = 1;
	fi->discrete.denominator = SUCAM_FPS;
	return 0;
}

static int sucam_parm(struct file *file, void *fh, struct v4l2_streamparm *p)
{
	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	memset(&p->parm.capture, 0, sizeof(p->parm.capture));
	p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	p->parm.capture.timeperframe.numerator = 1;
	p->parm.capture.timeperframe.denominator = SUCAM_FPS;
	p->parm.capture.readbuffers = 2;
	return 0;
}

static int sucam_enum_input(struct file *file, void *fh, struct v4l2_input *in)
{
	if (in->index)
		return -EINVAL;
	in->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(in->name, "Front camera", sizeof(in->name));
	return 0;
}

static int sucam_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int sucam_s_input(struct file *file, void *fh, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops sucam_ioctl_ops = {
	.vidioc_querycap = sucam_querycap,
	.vidioc_enum_fmt_vid_cap = sucam_enum_fmt,
	.vidioc_g_fmt_vid_cap = sucam_fmt,
	.vidioc_try_fmt_vid_cap = sucam_fmt,
	.vidioc_s_fmt_vid_cap = sucam_s_fmt,
	.vidioc_enum_framesizes = sucam_enum_framesizes,
	.vidioc_enum_frameintervals = sucam_enum_frameintervals,
	.vidioc_g_parm = sucam_parm,
	.vidioc_s_parm = sucam_parm,
	.vidioc_enum_input = sucam_enum_input,
	.vidioc_g_input = sucam_g_input,
	.vidioc_s_input = sucam_s_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations sucam_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* ---------- power management ---------- */
static int sucam_suspend(struct device *dev)
{
	struct sucam *cam = dev_get_drvdata(dev);

	mutex_lock(&cam->lock);
	sucam_hw_halt(cam);	/* sensor + LED off; buffers stay queued */
	mutex_unlock(&cam->lock);
	return 0;
}

static int sucam_resume(struct device *dev)
{
	struct sucam *cam = dev_get_drvdata(dev);

	mutex_lock(&cam->lock);
	/* the CCU island loses power across suspend; restart only if it is back */
	if (vb2_is_streaming(&cam->queue) && sucam_hw_run(cam)) {
		dev_warn(dev, "cannot resume streaming (CCU firmware gone?)\n");
		vb2_queue_error(&cam->queue);
	}
	mutex_unlock(&cam->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sucam_pm, sucam_suspend, sucam_resume);

/* ---------- probe ---------- */
static int sucam_probe(struct platform_device *pdev)
{
	struct sucam *cam;
	struct vb2_queue *q;
	int ret;

	cam = devm_kzalloc(&pdev->dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;
	cam->dev = &pdev->dev;
	mutex_init(&cam->lock);
	spin_lock_init(&cam->qlock);
	INIT_LIST_HEAD(&cam->queued);
	INIT_DELAYED_WORK(&cam->watchdog, sucam_watchdog);
	platform_set_drvdata(pdev, cam);

	/*
	 * camsv_b's SOF line, as listed in ISP1's _CRS. ISP1 has no driver, so
	 * map the GSI ourselves (this returns its existing mapping).
	 */
	cam->irq = acpi_register_gsi(NULL, SUCAM_SOF_GSI, ACPI_LEVEL_SENSITIVE,
				     ACPI_ACTIVE_HIGH);
	if (cam->irq < 0)
		dev_warn(cam->dev, "no SOF irq (%d), will poll\n", cam->irq);

	ret = sucam_hw_init(cam);
	if (ret)
		return dev_err_probe(cam->dev, ret, "hw init\n");

	ret = v4l2_device_register(cam->dev, &cam->v4l2_dev);
	if (ret)
		goto err_hw;

	v4l2_ctrl_handler_init(&cam->ctrls, 3);
	cam->exposure = v4l2_ctrl_new_std(&cam->ctrls, &sucam_ctrl_ops, V4L2_CID_EXPOSURE,
					  SUCAM_EXP_MIN, SUCAM_EXP_MAX, 1, SUCAM_EXP_DEF);
	cam->again = v4l2_ctrl_new_std(&cam->ctrls, &sucam_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
				       SUCAM_AGAIN_MIN, SUCAM_AGAIN_MAX, 1, SUCAM_AGAIN_DEF);
	cam->dgain = v4l2_ctrl_new_std(&cam->ctrls, &sucam_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
				       SUCAM_DGAIN_MIN, SUCAM_DGAIN_MAX, 1, SUCAM_DGAIN_DEF);
	if (cam->ctrls.error) {
		ret = cam->ctrls.error;
		goto err_ctrl;
	}
	cam->v4l2_dev.ctrl_handler = &cam->ctrls;

	q = &cam->queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;
	q->drv_priv = cam;
	q->buf_struct_size = sizeof(struct sucam_buf);
	q->ops = &sucam_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 1;
	q->lock = &cam->lock;
	q->dev = cam->dev;
	ret = vb2_queue_init(q);
	if (ret)
		goto err_ctrl;

	strscpy(cam->vdev.name, "Surface Laptop Ultra Camera", sizeof(cam->vdev.name));
	cam->vdev.v4l2_dev = &cam->v4l2_dev;
	cam->vdev.fops = &sucam_fops;
	cam->vdev.ioctl_ops = &sucam_ioctl_ops;
	cam->vdev.release = video_device_release_empty;
	cam->vdev.lock = &cam->lock;
	cam->vdev.queue = q;
	cam->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
				V4L2_CAP_READWRITE;
	video_set_drvdata(&cam->vdev, cam);
	ret = video_register_device(&cam->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_ctrl;

	dev_info(cam->dev, "%s registered (%ux%u MTK-packed GBRG10 @%u fps)\n",
		 video_device_node_name(&cam->vdev), SUCAM_W, SUCAM_H, SUCAM_FPS);
	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&cam->ctrls);
	v4l2_device_unregister(&cam->v4l2_dev);
err_hw:
	sucam_hw_exit(cam);
	return ret;
}

static void sucam_remove(struct platform_device *pdev)
{
	struct sucam *cam = platform_get_drvdata(pdev);

	video_unregister_device(&cam->vdev);
	v4l2_ctrl_handler_free(&cam->ctrls);
	v4l2_device_unregister(&cam->v4l2_dev);
	sucam_hw_exit(cam);
}

static struct platform_driver sucam_driver = {
	.probe = sucam_probe,
	.remove = sucam_remove,
	.driver = {
		.name = DRV_NAME,
		.pm = pm_sleep_ptr(&sucam_pm),
	},
};

static int __init sucam_init(void)
{
	struct platform_device_info info = {
		.name = DRV_NAME,
		.id = PLATFORM_DEVID_NONE,
		.dma_mask = DMA_BIT_MASK(32),	/* camsv sits behind an identity SMMU */
	};
	int ret;

	ret = platform_driver_register(&sucam_driver);
	if (ret)
		return ret;
	sucam_pdev = platform_device_register_full(&info);
	if (IS_ERR(sucam_pdev)) {
		platform_driver_unregister(&sucam_driver);
		return PTR_ERR(sucam_pdev);
	}
	return 0;
}

static void __exit sucam_exit(void)
{
	platform_device_unregister(sucam_pdev);
	platform_driver_unregister(&sucam_driver);
}

module_init(sucam_init);
module_exit(sucam_exit);
MODULE_DESCRIPTION("Surface Laptop Ultra front camera (OV02C10 / MediaTek ISP8 camsv)");
MODULE_LICENSE("GPL");
