package io.f411.ota;

import android.Manifest;
import android.app.*;
import android.bluetooth.*;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.*;
import android.provider.Settings;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;

public final class MainActivity extends Activity {
    private static final UUID SPP = UUID.fromString("00001101-0000-1000-8000-00805f9b34fb");
    private static final int BLUE = 0xff165c78, INK = 0xff152d39, MUTED = 0xff526974;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final ScheduledThreadPoolExecutor timer = new ScheduledThreadPoolExecutor(1);
    private LinearLayout body;
    private TextView boardText, firmwareText, status, log;
    private ProgressBar bar;
    private Button deviceBtn, retryBtn, cloudBtn, fileBtn, startBtn;
    private BluetoothSocket socket;
    private OtaLink link;
    private Firmware candidate;
    private String board, deviceName;
    private boolean cloudSource, busy, upgrading;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        timer.setRemoveOnCancelPolicy(true);
        getWindow().setStatusBarColor(INK);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        body = column(); body.setPadding(dp(20), dp(24), dp(20), dp(24));
        body.setBackgroundColor(0xffedf3f5); scroll.addView(body); setContentView(scroll);
        label(body,"F411 固件升级",30,INK,true);
        LinearLayout device = card("蓝牙设备");
        boardText = label(device,"尚未连接",17,INK,false);
        deviceBtn = button(device,"选择已配对设备",this::chooseDevice);
        retryBtn = button(device,"重新查询 APP 版本",()->run("正在查询 APP 版本",true,()->readVersion(link,false)));
        LinearLayout fw = card("升级固件");
        firmwareText = label(fw,"尚未选择固件",16,INK,false);
        cloudBtn = button(fw,"检查云端更新",this::download);
        fileBtn = button(fw,"选择本地 BIN",this::pickFile);
        LinearLayout transfer = card("升级进度");
        status = label(transfer,"等待操作",15,MUTED,false);
        bar = new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100); transfer.addView(bar);
        startBtn = button(transfer,"开始蓝牙升级",this::confirmInstall);
        log = label(transfer,"",13,MUTED,false); log.setTextIsSelectable(true);
        refresh();
    }

    private int dp(int v) { return Math.round(getResources().getDisplayMetrics().density*v); }
    private LinearLayout column() {
        LinearLayout box = new LinearLayout(this); box.setOrientation(LinearLayout.VERTICAL); return box;
    }
    private LinearLayout card(String name) {
        LinearLayout box = column();
        GradientDrawable panel = new GradientDrawable(); panel.setColor(Color.WHITE); panel.setCornerRadius(dp(14));
        box.setBackground(panel); box.setPadding(dp(16),dp(14),dp(16),dp(14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1,-2); lp.topMargin=dp(14); body.addView(box,lp);
        label(box,name,14,BLUE,true); return box;
    }
    private TextView label(LinearLayout parent,String text,int size,int color,boolean bold) {
        TextView t=new TextView(this); t.setText(text); t.setTextSize(size); t.setTextColor(color);
        if(bold) t.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        t.setPadding(0,dp(3),0,dp(5)); parent.addView(t); return t;
    }
    private Button button(LinearLayout parent,String title,Runnable click) {
        Button b=new Button(this); b.setText(title); b.setAllCaps(false);
        b.setOnClickListener(v -> click.run()); parent.addView(b); return b;
    }
    private void refresh() {
        deviceBtn.setText(socket == null ? "选择已配对设备" : "断开设备");
        boardText.setText(socket == null ? "尚未连接" : deviceName + "  ·  " +
                (link==null?"正在连接":board==null?"蓝牙已连接，版本待确认":board));
        retryBtn.setVisibility(!busy && link!=null && board==null ? View.VISIBLE : View.GONE);
        retryBtn.setEnabled(!busy);
        firmwareText.setText(candidate==null ? "尚无可用固件" :
                (cloudSource?"云端":"本地")+"  ·  "+candidate.version+"  ·  "+candidate.bytes.length+" 字节");
        deviceBtn.setEnabled(!busy); cloudBtn.setEnabled(!busy); fileBtn.setEnabled(!busy);
        startBtn.setEnabled(!busy && link!=null && board!=null && candidate!=null);
    }
    private void note(String text) {
        status.setText(text);
        String history=log.getText().toString();
        if(history.length()>1800) history=history.substring(history.indexOf('\n',500)+1);
        log.setText(history.isEmpty()?text:history+"\n"+text);
    }
    private void ui(Runnable action) {
        runOnUiThread(() -> { if (!isFinishing() && !isDestroyed()) action.run(); });
    }
    interface Task { Runnable run() throws Exception; }
    private void run(String title, boolean usesSocket, Task task) {
        busy=true; note(title); refresh();
        worker.execute(() -> {
            try {
                Runnable success=task.run();
                ui(() -> { success.run(); finished(); });
            } catch (Exception e) {
                ui(() -> { if(usesSocket) disconnect(); note(title+"失败："+e.getMessage()); finished(); });
            }
        });
    }
    private void finished() {
        busy=false; upgrading=false; getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); refresh();
    }
    private void disconnect() {
        if(socket!=null) try {socket.close();} catch(IOException ignored) {}
        socket=null; link=null; board=null;
    }
    private void chooseDevice() {
        if(socket!=null) { disconnect(); note("设备已断开"); refresh(); return; }
        if(Build.VERSION.SDK_INT>=31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)!=PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.BLUETOOTH_CONNECT},1); return;
        }
        try {
            BluetoothAdapter adapter=getSystemService(BluetoothManager.class).getAdapter();
            if(adapter==null){note("本机不支持蓝牙");return;}
            if(!adapter.isEnabled()){ startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)); return; }
            List<BluetoothDevice> devices=new ArrayList<>(adapter.getBondedDevices());
            String[] names=new String[devices.size()];
            for(int i=0;i<names.length;i++) names[i]=devices.get(i).getName()+"\n"+devices.get(i).getAddress();
            AlertDialog.Builder picker=new AlertDialog.Builder(this).setTitle("选择已配对的 JDY-31")
                    .setNeutralButton("系统蓝牙设置",(d,w)->startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)))
                    .setNegativeButton("取消",null);
            if(devices.isEmpty()) picker.setMessage("请先在系统设置里配对 JDY-31。");
            else picker.setItems(names,(d,w)->connect(devices.get(w)));
            picker.show();
        } catch(Exception e) {note("读取蓝牙设备失败："+e.getMessage());}
    }
    @Override public void onRequestPermissionsResult(int req,String[] permissions,int[] results) {
        super.onRequestPermissionsResult(req,permissions,results);
        if(req==1 && results.length>0 && results[0]==PackageManager.PERMISSION_GRANTED) chooseDevice();
        else note("连接已配对设备需要“附近设备”权限");
    }
    private void connect(BluetoothDevice device) {
        try {
            BluetoothSocket s=device.createRfcommSocketToServiceRecord(SPP);
            socket=s; deviceName=device.getName();
            run("连接蓝牙并查询 APP 版本",true,()->{
                OtaLink.bounded(timer,s,15,"蓝牙连接",s::connect);
                OtaLink connected=new OtaLink(s.getInputStream(),s.getOutputStream(),s,timer);
                  ui(()->{link=connected; note("蓝牙已连接，正在查询 APP 版本"); refresh();});
                  return readVersion(connected,true);
            });
        } catch(Exception e){disconnect();note("蓝牙连接失败："+e.getMessage());refresh();}
    }
    private Runnable readVersion(OtaLink connected, boolean firstConnection) throws IOException {
        try {
            String v=connected.queryConnected(firstConnection ?
                    ()->ui(()->note("首次查询未收到字节，已自动再发一次 V…")) : null);
            boolean outdated=cloudSource && candidate!=null && Firmware.code(candidate.version)<=Firmware.code(v);
            return ()->{ board=v; if(outdated) candidate=null; note("设备回复 APP_VERSION="+v); };
        } catch(java.net.SocketTimeoutException e) {
            return ()->{ board=null; note(e.getMessage()); };
        }
    }
    private void download() {
        candidate=null; cloudSource=true; bar.setProgress(0);
        String current=board;
        run("正在检查云端更新",false,()->{
            Firmware fw=Cloud.latest(current);
            return ()->{candidate=fw;note(fw==null?"设备已是最新版本":"云端固件校验通过，SHA-256 "+fw.sha);};
        });
    }
    private void pickFile() {
        Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        startActivityForResult(intent,2);
    }
    @Override protected void onActivityResult(int req,int result,Intent intent) {
        super.onActivityResult(req,result,intent);
        if(req!=2 || result!=RESULT_OK || intent==null || intent.getData()==null) return;
        Uri uri=intent.getData(); candidate=null; cloudSource=false; bar.setProgress(0);
        run("读取本地 BIN",false,()->{
            try(InputStream in=getContentResolver().openInputStream(uri)){
                Firmware fw=new Firmware(Firmware.read(in,Firmware.LIMIT));
                return ()->{candidate=fw; note("本地镜像已检查，SHA-256 "+fw.sha);};
            }
        });
    }
    private void confirmInstall() {
        if(busy || candidate==null || link==null || board==null) return;
        Firmware fw=candidate; OtaLink connected=link;
        new AlertDialog.Builder(this).setTitle("确认写入开发板？")
                .setMessage(deviceName+"\n"+board+" → "+fw.version+"\n"+fw.bytes.length+" 字节。请保持供电、蓝牙连接和应用前台运行。")
                .setNegativeButton("取消",null).setPositiveButton("开始升级",(d,w)->{
                    upgrading=true;
                    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    run("准备升级",true,()->{
                        if(cloudSource && Firmware.code(fw.version)<=Firmware.code(board))
                            throw new IOException("目标版本未高于设备版本");
                        connected.install(fw,(msg,percent)->ui(()->{status.setText(msg);bar.setProgress(percent);}));
                        return ()->{board=fw.version;candidate=null;note("实机已回复 APP_VERSION="+board);};
                    });
                }).show();
    }
    @Override public void onBackPressed(){
        if(upgrading){Toast.makeText(this,"升级期间请保持应用打开",Toast.LENGTH_SHORT).show();return;}
        super.onBackPressed();
    }
    @Override protected void onDestroy(){
        disconnect();worker.shutdownNow();timer.shutdownNow();super.onDestroy();
    }
}
