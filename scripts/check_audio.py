"""运行生产连续输入/平台打断与回复状态机的桌面回归；不连接设备或平台。"""
import argparse
from pathlib import Path
import shutil
import subprocess
from check_device_auth import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("tcc"), help="本机 TCC 路径")
    args = parser.parse_args()
    if not args.cc:
        raise SystemExit("请用 --cc 指定已安装的 TCC")
    root = Path(__file__).resolve().parents[1]
    output = root / "work/speech-handoff"
    output.mkdir(parents=True, exist_ok=True)
    # 直接编译 app_main 的生产交接函数，依赖由主机测试替换；不复制另一套流程。
    # 这些文件级函数的闭括号独占行。签名或结构变化时显式失败，要求同步测试入口。
    source = (root / "main/app_main.c").read_text(encoding="utf-8")
    sections = []
    for name in ("process_reply_offer", "process_platform_interruption"):
        start = source.index(f"static void {name}(void)\n{{")
        end = source.index("\n}\n", start) + 3
        sections.append(source[start:end])
    (output / "app_handoff.inc").write_text("\n\n".join(sections), encoding="utf-8")
    ws = (root / "main/ws_service.c").read_text(encoding="utf-8")
    end = ws.index("} rx_frame_t;") + len("} rx_frame_t;")
    start = ws.rindex("typedef struct {", 0, end)
    names = ("observe_rx_packet", "observe_control", "apply_platform_interruption_locked", "handle_frame",
             "finish_playback_if_drained", "ws_service_get_reply_offer", "ws_service_accept_reply",
             "reply_is", "ws_service_reply_interrupted", "ws_service_reply_finished")
    (output / "ws_platform.inc").write_text(ws[start:end] + "\n" +
        "\n\n".join(extract(ws, name) for name in names), encoding="utf-8")
    audio = (root / "main/audio_out.c").read_text(encoding="utf-8")
    (output / "audio_stop.inc").write_text("\n\n".join(extract(audio, name) for name in
        ("ramp_to_zero", "audio_out_write", "audio_out_stop", "audio_out_request_stop",
         "audio_out_stop_requested")), encoding="utf-8")
    cases = {
        "upload-clock-checks": ["tests/audio_host/upload_clock_checks.c"],
        "response-checks": ["-Itests/host", "-Imanaged_components/espressif__cjson/cJSON",
                            "tests/response_stream_checks.c", "main/proto_service.c",
                            "main/response_stream.c", "managed_components/espressif__cjson/cJSON/cJSON.c"],
        "upload-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                          "tests/audio_host/upload_checks.c"],
        "audio-stop-checks": ["-Itests/audio_host", "-Iwork/speech-handoff", "tests/audio_host/audio_stop_checks.c"],
        "ws-platform-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                               "-Imanaged_components/espressif__cjson/cJSON", "-Iwork/speech-handoff",
                               "tests/audio_host/ws_platform_checks.c", "main/response_stream.c",
                               "main/proto_service.c", "managed_components/espressif__cjson/cJSON/cJSON.c"],
        "app-handoff-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                               "-Iwork/speech-handoff", "tests/audio_host/app_handoff_checks.c"],
    }
    for name, sources in cases.items():
        executable = output / (name + ".exe")
        with (output / (name + ".log")).open("w", encoding="utf-8") as log:
            for command in ([args.cc, "-std=c11", "-Wall", "-Werror", "-o", str(executable), *sources],
                            [str(executable)]):
                result = subprocess.run(command, cwd=root, text=True, encoding="utf-8",
                                        errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                log.write(result.stdout)
                print(result.stdout, end="")
                if result.returncode:
                    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
