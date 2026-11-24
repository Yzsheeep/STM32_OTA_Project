package io.f411.ota;

import java.io.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** One caller owns this stream for an entire query or upgrade. */
final class OtaLink {
    interface Progress { void show(String message, int percent); }
    interface Step { void run() throws IOException; }
    private final InputStream in;
    private final OutputStream out;
    private final Closeable socket;
    private final ScheduledExecutorService timer;
    private String version;

    OtaLink(InputStream in, OutputStream out, Closeable socket, ScheduledExecutorService timer) {
        this.in = in; this.out = out; this.socket = socket; this.timer = timer;
    }

    static void bounded(ScheduledExecutorService timer, Closeable socket, int seconds,
                        String name, Step step) throws IOException {
        AtomicBoolean active = new AtomicBoolean(true);
        ScheduledFuture<?> alarm = timer.schedule(() -> {
            if (active.getAndSet(false)) {
                try { socket.close(); } catch (IOException ignored) { }
            }
        }, seconds, TimeUnit.SECONDS);
        try { step.run(); }
        finally {
            boolean completed = active.getAndSet(false);
            alarm.cancel(false);
            if (!completed) throw new IOException(name + "超时，连接已关闭");
        }
    }

    private void stage(int seconds, String name, Step work) throws IOException {
        bounded(timer, socket, seconds, name, work);
    }

    private void command(char c) throws IOException { out.write(c); out.flush(); }

    private void drain() throws IOException {
        int n = in.available();
        while (n-- > 0) next();
    }

    private int next() throws IOException {
        int value = in.read();
        if (value == -1) throw new EOFException("设备已断开");
        return value;
    }

    private String line() throws IOException {
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        for (;;) {
            int b = next();
            if (b == '\n') return data.toString("US-ASCII").trim();
            if (b != 0 && b != '\r') data.write(b);
            if (data.size() > 1024) throw new IOException("设备回复过长");
        }
    }

    private void ack(char want, String phase) throws IOException {
        int b = next();
        if (b != want) throw new IOException(phase + "：预期 " + want + "，收到 " +
                (b >= 32 && b < 127 ? Character.toString((char)b) : Integer.toString(b)));
    }

    String query() throws IOException {
        stage(3, "版本查询", () -> {
            drain(); command('V');
            for (;;) {
                String s = line();
                if (s.startsWith("APP_VERSION=")) {
                    version = s.substring("APP_VERSION=".length());
                    Firmware.code(version);
                    return;
                }
            }
        });
        return version;
    }

    // A missing APP reply during discovery must not tear down a usable SPP link.
    String queryConnected(Runnable onRetry) throws IOException {
        stage(3, "发送版本请求", () -> { drain(); command('V'); });
        long start = System.nanoTime();
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        int received = 0;
        boolean retried = false;
        long timeout = TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() - start < timeout) {
            if (in.available() == 0) {
                if (onRetry != null && !retried && received == 0 &&
                        System.nanoTime() - start >= TimeUnit.SECONDS.toNanos(2)) {
                    retried = true;
                    stage(3, "再次发送版本请求", () -> command('V'));
                    onRetry.run();
                    timeout = System.nanoTime() - start + TimeUnit.SECONDS.toNanos(5);
                }
                try { Thread.sleep(25); }
                catch (InterruptedException ex) {
                    Thread.currentThread().interrupt();
                    throw new IOException("查询已中止", ex);
                }
                continue;
            }
            int b = next();
            received++;
            if (b == '\n') {
                String s = line.toString("US-ASCII").trim();
                if (s.startsWith("APP_VERSION=")) {
                    String v = s.substring("APP_VERSION=".length()).trim();
                    Firmware.code(v);
                    return v;
                }
                line.reset();
            } else if (b != 0 && b != '\r' && line.size() < 1024) {
                line.write(b);
            }
        }
        throw new java.net.SocketTimeoutException("已发送 " + (retried ? "2" : "1") + " 次 V，共收到 " + received
                + " 字节，未读到完整 APP 版本。可点击重新查询。");
    }

    void install(Firmware fw, Progress progress) throws IOException {
        progress.show("等待 Bootloader", 0);
        stage(5, "进入升级模式", () -> {
            drain(); command('U');
            for (;;) {
                String s = line();
                if (s.equals("Wait for OTA update...") || s.equals("Set Flag OK!")) return;
                if (s.contains("failed") || s.contains("Err!") || s.contains("not ready"))
                    throw new IOException(s);
            }
        });
        stage(3, "元数据握手", () -> { command('H'); ack('h', "元数据握手"); });
        stage(3, "保存元数据", () -> { out.write(fw.metadata()); out.flush(); ack('H', "保存元数据"); });
        progress.show("擦除外部暂存区", 0);
        stage(10, "擦除", () -> { command('E'); ack('E', "擦除"); });
        for (int pos = 0; pos < fw.bytes.length; pos += 128) {
            final int offset = pos, size = Math.min(128, fw.bytes.length - pos);
            String packet = "数据包 " + (pos / 128 + 1) + "/" + ((fw.bytes.length + 127) / 128);
            stage(3, packet, () -> {
                out.write(fw.bytes, offset, size); out.flush(); ack('0', packet);
            });
            progress.show(packet + " 已确认", (pos + size) * 100 / fw.bytes.length);
        }
        progress.show("传输完成，等待设备安装和启动", 100);
        try { Thread.sleep(500); }  // 与 Windows 端发 C 前的间隔一致。
        catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IOException("升级已中断", ex);
        }
        stage(90, "安装及启动", () -> {
            drain(); command('C');
            boolean managed = false, confirmed = false, started = false;
            for (;;) {
                String s = line();
                if (s.contains("failed") || s.contains("Failed") || s.contains("Err!") ||
                        s.contains("OTA rolled back") || s.contains("OTA recovery stopped"))
                    throw new IOException(s);
                managed |= s.equals("OTA managed") || s.equals("OTA trial");
                confirmed |= s.equals("OTA confirmed");
                started |= s.equals("Hello! I am App in 2nd Floor!");
                if (started && (!managed || confirmed)) break;
            }
        });
        String actual = query();
        if (!fw.version.equals(actual)) throw new IOException("启动版本 " + actual + " 与目标 " + fw.version + " 不符");
        progress.show("升级完成，APP 已回复 " + actual, 100);
    }
}
