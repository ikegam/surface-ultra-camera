/*
 * ccusnoop v4 - Surface Laptop Ultra (N1X) CCU mailbox snooper + full camera register dumper.
 *
 * 1) Polls both CCU mailboxes (0x3C0BC0C8 / 0x3C0CC0C8), logs every command (ft/cmd/payload).
 * 2) Once the camera becomes active (first mailbox command), every ~1s (and on unload) dumps
 *    the WHOLE direct-MMIO camera path: CSI ports 0-2 (rxanaA/B, dphy, mac_top, mac_csi),
 *    seninf-top, async, outmux0..13, camsv_a/b/e. Snapshots taken while a camera streams are
 *    the golden config to replay. v4 adds ports 1/2 and camsv_e for the IR camera (port 2);
 *    those are only read after camera activity, in case an unclocked block stalls the bus.
 *
 * Output -> C:\ccusnoop.log (also DbgPrint).
 *   sc create ccusnoop type= kernel binPath= C:\ccusnoop\...\ccusnoop.sys
 *   sc start ccusnoop ; open Camera, PREVIEW and KEEP IT OPEN ; then: sc stop ccusnoop
 *   (keeping the camera open at 'sc stop' makes the final dump a streaming-state snapshot)
 */
#include <ntddk.h>
#include <ntstrsafe.h>

#define CCU_BASE 0x3C080000ULL
#define CCU_SIZE 0x50000
#define H_WIDX 0x00
#define H_FT   0x08
#define H_CMD  0x0C
#define H_LEN  0x14
#define H_STAT 0x18
#define PL_MAX 0x11C
#define LOGSZ (4 * 1024 * 1024)

#define SENINF_BASE 0x12A10000ULL   /* per port (stride 0x8000): rxanaA@0 rxanaB@0x1000 dphy@0x2000 mac_top@0x4000 mac_csi@0x5000 */
#define SENINF_SIZE 0x18000
#define PORT_STRIDE 0x8000
#define TOP_BASE    0x3A300000ULL   /* seninf-top@0 async@0x1000 tg@0x2000 */
#define TOP_SIZE    0x3000
#define OMUX_BASE   0x3A100000ULL   /* outmux0..13 stride 0x10000 */
#define OMUX_SIZE   0xE0000
#define CAMSV_BASE  0x3A500000ULL   /* camsv_a@0 camsv_b@0x10000 camsv_e@0x40000 */
#define CAMSV_SIZE  0x50000

static PVOID g_ccu, g_seninf, g_top, g_omux, g_camsv;
static PKTHREAD g_thread;
static BOOLEAN g_stop;
static char *g_log;
static ULONG g_logpos;
static int g_dumpseq;
static BOOLEAN g_active;		/* a mailbox command has been seen: camera blocks are clocked */

static const ULONG MB_HDR[2] = { 0x3C0C8, 0x4C0C8 };
static const ULONG MB_PL[2]  = { 0x3C0E4, 0x4C0E4 };

static void logline(const char *s)
{
	size_t n = 0;

	RtlStringCbLengthA(s, 500, &n);
	if (g_logpos + n + 2 < LOGSZ) {
		RtlCopyMemory(g_log + g_logpos, s, n);
		g_logpos += (ULONG)n;
		g_log[g_logpos++] = '\r';
		g_log[g_logpos++] = '\n';
	}
}

static ULONG rd(PVOID base, ULONG off)
{
	return READ_REGISTER_ULONG((PULONG)((volatile UCHAR *)base + off));
}

/* dump `count` u32 from base+start, 8 per line, skipping all-zero lines to stay compact */
static void dumpblk(const char *tag, PVOID base, ULONG start, ULONG count)
{
	char buf[256];
	ULONG i, j;

	if (!base)
		return;
	for (i = 0; i < count; i += 8) {
		size_t off = 0;
		ULONG nz = 0, v[8];

		for (j = 0; j < 8 && i + j < count; j++) {
			v[j] = rd(base, start + (i + j) * 4);
			if (v[j])
				nz = 1;
		}
		if (!nz)
			continue;
		RtlStringCbPrintfA(buf, sizeof(buf), "  %s+%03x:", tag, start + i * 4);
		for (j = 0; j < 8 && i + j < count; j++) {
			RtlStringCbLengthA(buf, sizeof(buf), &off);
			RtlStringCbPrintfA(buf + off, sizeof(buf) - off, " %08x", v[j]);
		}
		logline(buf);
	}
}

static void dump_regs(void)
{
	char hdr[64];

	RtlStringCbPrintfA(hdr, sizeof(hdr), "=== DUMP #%d ===", g_dumpseq++);
	logline(hdr);
	/* CSI ports: port 0 always (as in v3), ports 1/2 only once the camera is active */
	{
		int p;

		for (p = 0; p < 3; p++) {
			ULONG b = (ULONG)p * PORT_STRIDE;
			char t[16];

			if (p && !g_active)
				break;
			RtlStringCbPrintfA(t, sizeof(t), "p%d.rxanaA", p);
			dumpblk(t, g_seninf, b + 0x0000, 0x40);
			dumpblk(t, g_seninf, b + 0x00f0, 0x8);
			dumpblk(t, g_seninf, b + 0x0100, 0x8);
			RtlStringCbPrintfA(t, sizeof(t), "p%d.rxanaB", p);
			dumpblk(t, g_seninf, b + 0x1000, 0x40);
			dumpblk(t, g_seninf, b + 0x10f0, 0x8);
			RtlStringCbPrintfA(t, sizeof(t), "p%d.dphy", p);
			dumpblk(t, g_seninf, b + 0x2000, 0x80);
			RtlStringCbPrintfA(t, sizeof(t), "p%d.mactop", p);
			dumpblk(t, g_seninf, b + 0x4000, 0x40);
			RtlStringCbPrintfA(t, sizeof(t), "p%d.maccsi", p);
			dumpblk(t, g_seninf, b + 0x5000, 0x80);
		}
	}
	dumpblk("sntop",  g_top,    0x0000, 0x40);
	dumpblk("async",  g_top,    0x1000, 0x40);
	dumpblk("tg",     g_top,    0x2000, 0x20);
	/* all 14 outmuxes */
	{
		int n;

		for (n = 0; n < 14; n++) {
			char t[16];

			RtlStringCbPrintfA(t, sizeof(t), "omux%d", n);
			dumpblk(t, g_omux, (ULONG)n * 0x10000, 0x40);
			dumpblk(t, g_omux, (ULONG)n * 0x10000 + 0x80, 0x10);
		}
	}
	/* camsv_a/b (as in v3) and camsv_e (after activity): central + DMATOP + SCQ */
	dumpblk("camsv0",     g_camsv, 0x0000, 0x180);
	dumpblk("camsv0dma",  g_camsv, 0x1000, 0x100);
	dumpblk("camsv0scq",  g_camsv, 0x2000, 0x40);
	dumpblk("camsv1",     g_camsv, 0x10000, 0x180);
	dumpblk("camsv1dma",  g_camsv, 0x11000, 0x100);
	dumpblk("camsv1scq",  g_camsv, 0x12000, 0x40);
	if (g_active) {
		dumpblk("camsv4",     g_camsv, 0x40000, 0x180);
		dumpblk("camsv4dma",  g_camsv, 0x41000, 0x100);
		dumpblk("camsv4scq",  g_camsv, 0x42000, 0x40);
	}
	/* outmux0,1 wider (0..0x200) */
	dumpblk("omux0w", g_omux, 0x00, 0x80);
	dumpblk("omux1w", g_omux, 0x10000, 0x80);
	logline("=== END ===");
}

static VOID PollThread(PVOID ctx)
{
	volatile UCHAR *base = (volatile UCHAR *)g_ccu;
	ULONG lastw[2] = { 0xFFFFFFFF, 0xFFFFFFFF };
	LONGLONG iter = 0, next_dump = 0;
	char buf[400];
	int m;

	UNREFERENCED_PARAMETER(ctx);
	logline("ccusnoop v4: poll start");
	while (!g_stop) {
		for (m = 0; m < 2; m++) {
			ULONG hdr = MB_HDR[m];
			ULONG w = rd((PVOID)base, hdr + H_WIDX);

			if (w != lastw[m]) {
				ULONG ft = rd((PVOID)base, hdr + H_FT);
				ULONG cmd = rd((PVOID)base, hdr + H_CMD);
				ULONG len = rd((PVOID)base, hdr + H_LEN);
				ULONG st = rd((PVOID)base, hdr + H_STAT);
				ULONG i;
				size_t off = 0;

				if (len > PL_MAX)
					len = PL_MAX;
				RtlStringCbPrintfA(buf, sizeof(buf),
					"mb%d w=%u ft=%u cmd=0x%x len=%u st=0x%08x pl:", m, w, ft, cmd, len, st);
				for (i = 0; i < len && i < 72; i++) {
					RtlStringCbLengthA(buf, sizeof(buf), &off);
					RtlStringCbPrintfA(buf + off, sizeof(buf) - off, " %02x",
						READ_REGISTER_UCHAR(base + MB_PL[m] + i));
				}
				logline(buf);
				DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "ccusnoop: %s\n", buf);
				if (lastw[m] != 0xFFFFFFFF && !g_active) {
					g_active = TRUE;		/* first real command: start dumping */
					next_dump = iter + 400000;	/* ~0.5s, let stream-on settle */
				}
				lastw[m] = w;
			}
		}
		/* one dump at load, then every ~1s once the camera is active, up to 30 times */
		if (iter >= next_dump && g_dumpseq < 30 && (g_active || g_dumpseq == 0)) {
			dump_regs();
			next_dump = iter + 800000;
		}
		iter++;
		KeStallExecutionProcessor(1);
	}
	logline("ccusnoop v4: poll stop");
}

static void flush_file(void)
{
	UNICODE_STRING name;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	HANDLE h;
	NTSTATUS s;

	RtlInitUnicodeString(&name, L"\\??\\C:\\ccusnoop.log");
	InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
	s = ZwCreateFile(&h, GENERIC_WRITE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
		0, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
	if (NT_SUCCESS(s)) {
		ZwWriteFile(h, NULL, NULL, NULL, &iosb, g_log, g_logpos, NULL, NULL);
		ZwClose(h);
	}
}

static VOID Unload(PDRIVER_OBJECT drv)
{
	UNREFERENCED_PARAMETER(drv);
	g_stop = TRUE;
	if (g_thread) {
		KeWaitForSingleObject(g_thread, Executive, KernelMode, FALSE, NULL);
		ObDereferenceObject(g_thread);
	}
	g_dumpseq = 99;			/* mark the final snapshot */
	dump_regs();
	flush_file();
	if (g_ccu) MmUnmapIoSpace(g_ccu, CCU_SIZE);
	if (g_seninf) MmUnmapIoSpace(g_seninf, SENINF_SIZE);
	if (g_top) MmUnmapIoSpace(g_top, TOP_SIZE);
	if (g_omux) MmUnmapIoSpace(g_omux, OMUX_SIZE);
	if (g_camsv) MmUnmapIoSpace(g_camsv, CAMSV_SIZE);
	if (g_log) ExFreePool(g_log);
}

static PVOID mapio(ULONGLONG pa, ULONG sz)
{
	PHYSICAL_ADDRESS p;

	p.QuadPart = pa;
	return MmMapIoSpaceEx(p, sz, PAGE_READWRITE | PAGE_NOCACHE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
	HANDLE h;
	NTSTATUS s;

	UNREFERENCED_PARAMETER(reg);
	drv->DriverUnload = Unload;

	g_log = (char *)ExAllocatePool2(POOL_FLAG_NON_PAGED, LOGSZ, 'uccs');
	if (!g_log)
		return STATUS_INSUFFICIENT_RESOURCES;
	g_ccu    = mapio(CCU_BASE, CCU_SIZE);
	g_seninf = mapio(SENINF_BASE, SENINF_SIZE);
	g_top    = mapio(TOP_BASE, TOP_SIZE);
	g_omux   = mapio(OMUX_BASE, OMUX_SIZE);
	g_camsv  = mapio(CAMSV_BASE, CAMSV_SIZE);
	if (!g_ccu) {
		ExFreePool(g_log);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	g_stop = FALSE;
	s = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, NULL);
	if (NT_SUCCESS(s)) {
		ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&g_thread, NULL);
		ZwClose(h);
	}
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
		"ccusnoop v4: loaded ccu=%p seninf=%p top=%p omux=%p camsv=%p\n",
		g_ccu, g_seninf, g_top, g_omux, g_camsv);
	return STATUS_SUCCESS;
}
