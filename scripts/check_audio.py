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
    (output / "app_reminder.inc").write_text("\n\n".join(extract(source, name) for name in
        ("end_conversation", "poll_reminder")), encoding="utf-8")
    ws = (root / "main/ws_service.c").read_text(encoding="utf-8")
    end = ws.index("} rx_frame_t;") + len("} rx_frame_t;")
    start = ws.rindex("typedef struct {", 0, end)
    names = ("observe_rx_packet", "observe_control", "apply_platform_interruption_locked", "handle_frame",
             "finish_playback_if_drained", "ws_service_get_reply_offer", "ws_service_accept_reply",
             "reply_is", "ws_service_reply_interrupted", "ws_service_reply_finished",
             "ws_service_prepare_reminder", "ws_service_reminder_timed_out")
    (output / "ws_platform.inc").write_text(ws[start:end] + "\n" +
        "\n\n".join(extract(ws, name) for name in names), encoding="utf-8")
    (output / "ws_upload.inc").write_text(extract(ws, "ws_service_send_opus_frame"), encoding="utf-8")
    wifi = (root / "main/wifi_service.c").read_text(encoding="utf-8")
    (output / "wifi_audio.inc").write_text("\n\n".join(extract(wifi, name) for name in
        ("apply_audio_power_save_locked", "wifi_service_set_audio_active",
         "wifi_event_handler")), encoding="utf-8")
    audio = (root / "main/audio_out.c").read_text(encoding="utf-8")
    (output / "audio_stop.inc").write_text("\n\n".join(extract(audio, name) for name in
        ("ramp_to_zero", "audio_out_write", "audio_out_stop", "audio_out_request_stop",
         "audio_out_stop_requested")), encoding="utf-8")
    (output / "audio_signal.inc").write_text("\n\n".join(extract(audio, name) for name in
        ("measure_pcm_signal", "audio_out_set_streaming", "audio_out_play_opus")), encoding="utf-8")
    wake = (root / "main/wake_word.c").read_text(encoding="utf-8")
    (output / "wake_fifo.inc").write_text("\n\n".join(extract(wake, name) for name in
        ("audio_buffer_frames_locked", "discard_oldest_audio_locked", "clear_audio_buffer_locked",
         "afe_frame_energy", "buffer_afe_audio", "wake_word_set_audio_buffer_enabled")), encoding="utf-8")
    cases = {
        "wifi-audio-checks": ["-Itests/audio_host", "-Iwork/speech-handoff",
                              "tests/audio_host/wifi_audio_checks.c"],
        "ws-upload-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                             "-Imanaged_components/espressif__cjson/cJSON", "-Iwork/speech-handoff",
                             "tests/audio_host/ws_upload_checks.c", "main/proto_service.c",
                             "managed_components/espressif__cjson/cJSON/cJSON.c"],
        "response-checks": ["-Itests/host", "-Imanaged_components/espressif__cjson/cJSON",
                            "tests/response_stream_checks.c", "main/proto_service.c",
                            "main/response_stream.c", "managed_components/espressif__cjson/cJSON/cJSON.c"],
        "upload-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                          "tests/audio_host/upload_checks.c"],
        "audio-stop-checks": ["-Itests/audio_host", "-Iwork/speech-handoff", "tests/audio_host/audio_stop_checks.c"],
        "audio-signal-checks": ["-Itests/audio_host", "-Iwork/speech-handoff", "tests/audio_host/audio_signal_checks.c"],
        "wake-fifo-checks": ["-Itests/audio_host", "-Iwork/speech-handoff", "tests/audio_host/wake_fifo_checks.c"],
        "ws-platform-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                               "-Imanaged_components/espressif__cjson/cJSON", "-Iwork/speech-handoff",
                               "tests/audio_host/ws_platform_checks.c", "main/response_stream.c",
                               "main/proto_service.c", "managed_components/espressif__cjson/cJSON/cJSON.c"],
        "app-handoff-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                               "-Iwork/speech-handoff", "tests/audio_host/app_handoff_checks.c"],
        "app-reminder-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                                "-Iwork/speech-handoff", "tests/audio_host/app_reminder_checks.c"],
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
