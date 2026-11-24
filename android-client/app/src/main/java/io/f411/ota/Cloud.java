package io.f411.ota;

import java.io.*;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import javax.net.ssl.HttpsURLConnection;
import org.json.JSONObject;

final class Cloud {
    static final String ORIGIN = "https://47.107.50.1";
    static final String PATH = "/api/v1/targets/f411-ota";

    private static byte[] get(String path, int max) throws IOException {
        HttpsURLConnection c = (HttpsURLConnection) new URL(ORIGIN + path).openConnection();
        try {
            c.setConnectTimeout(15000);
            c.setReadTimeout(15000);
            c.setUseCaches(false);
            c.setInstanceFollowRedirects(false);
            c.setRequestProperty("Accept-Encoding", "identity");
            int status = c.getResponseCode();
            if (status != 200) throw new IOException("固件服务 HTTP " + status);
            try (InputStream in = c.getInputStream()) { return Firmware.read(in, max); }
        } finally { c.disconnect(); }
    }

    static Firmware latest(String board) throws Exception {
        JSONObject m = new JSONObject(new String(get(PATH + "/latest", 16384), StandardCharsets.UTF_8));
        String version = m.getString("latest_version");
        int code = Firmware.code(version);
        String file = PATH + "/releases/" + version + "/firmware.bin";
        Object length = m.get("size");
        if (!(length instanceof Integer)) throw new IOException("清单长度无效");
        int size = (Integer) length;
        String crc = m.getString("crc32"), sha = m.getString("sha256");
        if (!"f411-ota".equals(m.getString("target")) || !"0x08010000".equals(m.getString("app_address"))
                || !file.equals(m.getString("download_url")) || size < 8 || size > Firmware.LIMIT
                || size % 4 != 0 || !crc.matches("[a-fA-F0-9]{8}") || !sha.matches("[a-fA-F0-9]{64}"))
            throw new IOException("F411 发布清单无效");
        if (board != null && code <= Firmware.code(board)) return null;
        Firmware fw = new Firmware(get(file, size));
        if (fw.bytes.length != size || !fw.version.equals(version) || !fw.sha.equalsIgnoreCase(sha)
                || !String.format(Locale.ROOT,"%08X",fw.crc).equalsIgnoreCase(crc))
            throw new IOException("下载内容与发布清单不符");
        return fw;
    }
}
