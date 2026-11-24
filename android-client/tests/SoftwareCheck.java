package io.f411.ota;

import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Desktop-only checks. No Android device, Bluetooth adapter or serial port is used. */
public class SoftwareCheck {
    static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    public static void main(String[] args) throws Exception {
        Firmware fw=new Firmware(Files.readAllBytes(Paths.get(args[0])));
        require(Firmware.code("1.0.10")>Firmware.code("1.0.9"),"numeric versions");
        for(int field : new int[]{0,4}) {
            byte[] bad=fw.bytes.clone(); Arrays.fill(bad,field,field+4,(byte)0);
            try {new Firmware(bad);throw new AssertionError("bad vector accepted");} catch(IOException expected){}
        }
        try {Firmware.read(new ByteArrayInputStream(new byte[16385]),16384);throw new AssertionError("oversize accepted");}
        catch(IOException expected){}
        System.out.println("Firmware checks passed.");
        if(args.length>1 && args[1].equals("--public")){
            Firmware downloaded=Cloud.latest(null);
            require(Arrays.equals(fw.bytes,downloaded.bytes),"download differs from reference");
            require(Cloud.latest(downloaded.version)==null,"same version");
            System.out.println("Public HTTPS verified: "+fw.version+" "+fw.bytes.length+" SHA256 "+fw.sha);
        }
    }
}
