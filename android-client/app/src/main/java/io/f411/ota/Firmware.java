package io.f411.ota;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.regex.*;
import java.util.zip.CRC32;

final class Firmware {
    static final int LIMIT = 16384;
    static final long BASE = 0x08010000L;
    private static final Pattern MARKER = Pattern.compile("APP_VERSION=([0-9]+\\.[0-9]+\\.[0-9]+)\r\n\u0000");
    final byte[] bytes;
    final String version, sha;
    final int crc;

    static int code(String v) throws IOException {
        if (!v.matches("(0|[1-9][0-9]{0,2})(\\.(0|[1-9][0-9]{0,2})){2}"))
            throw new IOException("版本格式无效");
        int value = 0;
        for (String part : v.split("\\.")) {
            int n = Integer.parseInt(part);
            if (n > 255) throw new IOException("版本分量超过 255");
            value = value * 256 + n;
        }
        return value;
    }

    static byte[] read(InputStream in, int limit) throws IOException {
        if (in == null) throw new IOException("无法打开数据流");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int count;
        while ((count = in.read(buffer)) != -1) {
            if (out.size() + count > limit) throw new IOException("数据超过允许长度");
            out.write(buffer, 0, count);
        }
        return out.toByteArray();
    }

    Firmware(byte[] data) throws Exception {
        if (data.length < 8 || data.length > LIMIT || data.length % 4 != 0)
            throw new IOException("BIN 长度无效，应为 8～16384 且按 4 字节对齐");
        ByteBuffer vector = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        long sp = Integer.toUnsignedLong(vector.getInt()), entry = Integer.toUnsignedLong(vector.getInt());
        if (sp <= 0x20000000L || sp > 0x20020000L || sp % 8 != 0 ||
                entry % 2 == 0 || entry < BASE || entry >= BASE + data.length)
            throw new IOException("F411 APP 向量表无效");
        Matcher marker = MARKER.matcher(new String(data, StandardCharsets.ISO_8859_1));
        if (!marker.find()) throw new IOException("未找到 APP_VERSION");
        version = marker.group(1);
        code(version);
        while (marker.find()) if (!version.equals(marker.group(1))) throw new IOException("BIN 版本不唯一");
        bytes = data;
        CRC32 sum = new CRC32();
        sum.update(bytes);
        crc = (int) sum.getValue();
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(bytes))
            hex.append(String.format(Locale.ROOT, "%02x", b & 255));
        sha = hex.toString();
    }

    byte[] metadata() throws IOException {
        return ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(0x55AA55AA).putInt(bytes.length).putInt(crc).putInt(code(version)).array();
    }
}
