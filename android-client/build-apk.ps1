# Builds a sideloadable test APK with the official SDK tools, without Gradle.
param(
    [string]$Jdk = $env:JAVA_HOME,
    [string]$Platform = "$env:ANDROID_HOME/platforms/android-34",
    [string]$BuildTools = "$env:ANDROID_HOME/build-tools/34.0.0"
)
$ErrorActionPreference = 'Stop'
function Invoke-Checked($exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe exited with $LASTEXITCODE" }
}
$root = $PSScriptRoot
$src = "$root/app/src/main"
$build = "$root/app/build"
$work = "$build/sdk/$([guid]::NewGuid().ToString('N'))"
$out = "$build/outputs/apk/debug"
$keyDir = "$env:LOCALAPPDATA/F411-OTA"
$key = "$keyDir/debug.jks"
$androidJar = "$Platform/android.jar"
foreach ($file in @("$Jdk/bin/javac.exe", $androidJar, "$BuildTools/aapt2.exe", "$BuildTools/lib/d8.jar")) {
    if (!(Test-Path -LiteralPath $file)) { throw "Missing $file" }
}
New-Item -ItemType Directory -Force "$work/classes","$work/dex",$out,$keyDir | Out-Null
$manifest = [IO.File]::ReadAllText("$src/AndroidManifest.xml").Replace('<manifest ', '<manifest package="io.f411.ota" ')
[IO.File]::WriteAllText("$work/AndroidManifest.xml",$manifest)
Invoke-Checked "$BuildTools/aapt2.exe" @('link','-I',$androidJar,'--manifest',"$work/AndroidManifest.xml",
    '--min-sdk-version','26','--target-sdk-version','34','--version-code','3','--version-name','1.2','-o',"$work/base.apk")
$sources = @(Get-ChildItem "$src/java" -Recurse -Filter '*.java' | ForEach-Object FullName)
Invoke-Checked "$Jdk/bin/javac.exe" (@('-encoding','UTF-8','-source','8','-target','8','-bootclasspath',"$androidJar;$BuildTools/core-lambda-stubs.jar",'-d',"$work/classes") + $sources)
Invoke-Checked "$Jdk/bin/jar.exe" @('cf',"$work/app.jar",'-C',"$work/classes",'.')
Invoke-Checked "$Jdk/bin/java.exe" @('-cp',"$BuildTools/lib/d8.jar",'com.android.tools.r8.D8','--lib',$androidJar,
    '--min-api','26','--output',"$work/dex","$work/app.jar")
Invoke-Checked "$Jdk/bin/jar.exe" @('uf',"$work/base.apk",'-C',"$work/dex",'classes.dex')
Invoke-Checked "$BuildTools/zipalign.exe" @('-f','4',"$work/base.apk","$work/aligned.apk")
if (!(Test-Path $key)) {
    Invoke-Checked "$Jdk/bin/keytool.exe" @('-genkeypair','-keystore',$key,'-storepass','android','-keypass','android',
        '-alias','f411debug','-dname','CN=F411 Local Debug','-keyalg','RSA','-keysize','2048','-validity','10000')
}
$apk = "$out/F411-OTA-debug.apk"
$signer = "$BuildTools/lib/apksigner.jar"
Invoke-Checked "$Jdk/bin/java.exe" @('-jar',$signer,'sign','--ks',$key,'--ks-key-alias','f411debug',
    '--ks-pass','pass:android','--key-pass','pass:android','--out',"$work/signed.apk","$work/aligned.apk")
Invoke-Checked "$Jdk/bin/java.exe" @('-jar',$signer,'verify','--verbose',"$work/signed.apk")
Copy-Item -LiteralPath "$work/signed.apk" -Destination $apk
Get-FileHash -LiteralPath $apk
