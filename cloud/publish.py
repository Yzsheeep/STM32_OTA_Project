"""Publish the Keil F411 image through an existing local SSH tunnel."""
import argparse
import getpass
import hashlib
import json
import re
import sys
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener

BASE = "http://127.0.0.1:18080/api/v1/targets/f411-ota"
DEFAULT_BIN = Path(__file__).resolve().parents[1] / "app_f411/MDK-ARM/app_f411/app_f411.bin"


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin", type=Path, nargs="?", default=DEFAULT_BIN)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    image = args.bin.read_bytes()
    versions = set(re.findall(rb"APP_VERSION=(\d+\.\d+\.\d+)\r\n\x00", image))
    if len(versions) != 1:
        raise ValueError("BIN 必须包含唯一的 APP_VERSION")
    version = versions.pop().decode("ascii")
    digest = hashlib.sha256(image).hexdigest()
    print(f"{args.bin}\n版本 {version}，{len(image)} 字节\nSHA-256 {digest}")
    if args.dry_run:
        return
    api = build_opener(ProxyHandler({}), NoRedirect())
    token = getpass.getpass("管理员令牌（隐藏输入）：").strip()
    if not token:
        raise ValueError("管理员令牌为空")

    def post(path, body):
        req = Request(BASE + path, data=body, method="POST", headers={
            "Authorization": "Bearer " + token, "Content-Type": "application/octet-stream"})
        with api.open(req, timeout=20) as response:
            return json.load(response)

    draft = post("/admin/releases?version=" + version, image)
    if draft.get("version") != version or draft.get("state") != "draft":
        raise ValueError("上传响应异常，请检查服务器草稿状态")
    print("已上传草稿，正在发布…")
    manifest = post("/admin/releases/" + version + "/publish", b"")
    if (manifest.get("latest_version"), manifest.get("size"), manifest.get("sha256")) != (
            version, len(image), digest):
        raise ValueError("服务端发布清单与本地 BIN 不符，请核对发布状态")
    print(f"已发布 {version}，服务端 SHA-256 与本地一致")


if __name__ == "__main__":
    try:
        main()
    except HTTPError as exc:
        sys.exit(f"HTTP {exc.code}：" + exc.read(512).decode(errors="replace"))
    except (OSError, URLError, ValueError) as exc:
        sys.exit(str(exc))
