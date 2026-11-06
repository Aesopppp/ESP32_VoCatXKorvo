"""打包选定的原版 EAF，不转换图像、不下载依赖；使用 ESP-IDF 自带 Python。

VFACE01\0 + uint32(total_size, count)，随后 count 个 (offset, size) 小端表项。
四个文件按 manifest 顺序紧密拼接。原 EAF 自带完整 payload 校验和。
运行时格式见 main/ui_animation.c；资源包与应用分别烧录。
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def build_pack(directory: Path) -> bytes:
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    names = [item["file"] for item in manifest["animations"]]
    if manifest["format_version"] != 1 or manifest["fps"] != 20 or names != [
        "neutral.eaf", "winking.eaf", "confused.eaf", "listen.eaf"
    ]:
        raise ValueError("资源版本、帧率或枚举顺序与固件不匹配")
    files = []
    for item in manifest["animations"]:
        data = (directory / item["file"]).read_bytes()
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError(f"资源校验失败：{item['file']}，更换资源时须核对并更新 manifest")
        files.append(data)
    offset = 16 + 8 * len(files)
    table = bytearray()
    for data in files:
        table.extend(struct.pack("<II", offset, len(data)))
        offset += len(data)
    return b"VFACE01\0" + struct.pack("<II", offset, len(files)) + table + b"".join(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-size", type=lambda n: int(n, 0), default=0x100000)
    args = parser.parse_args()
    data = build_pack(args.source)
    if len(data) > args.max_size:
        raise SystemExit(f"表情资源 {len(data)} bytes 超出 ui_assets 分区 {args.max_size} bytes")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"UI assets: {len(data)} bytes / {args.max_size} bytes, version=1, fps=20")


if __name__ == "__main__":
    main()
